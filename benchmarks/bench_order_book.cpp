#include "order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// TIMING RATIONALE:
// Why std::chrono::steady_clock is strictly required here over high_resolution_clock:
//
// std::chrono::steady_clock is guaranteed to be monotonic, meaning its clock tick
// count increases at a physical rate and NEVER jumps backward or forward due to system
// clock adjustments (such as NTP time sync, daylight saving shifts, or manual clock changes).
//
// In contrast, std::chrono::high_resolution_clock is implementation-defined in standard C++.
// On many systems (e.g. MSVC or GCC on certain standard library implementations),
// high_resolution_clock is an alias for system_clock, which is wall-clock time and is NOT
// monotonic. Using a non-monotonic clock can result in distorted, zero, or negative time
// duration measurements.
//
// Therefore, steady_clock is the only strictly correct choice for reliable micro-benchmarking.
// -----------------------------------------------------------------------------

constexpr uint32_t RANDOM_SEED = 12345;
constexpr double MID_PRICE = 100.0;

struct OrderGenerator {
    std::mt19937 rng{RANDOM_SEED};
    std::uniform_int_distribution<int> side_dist{0, 1};
    std::uniform_real_distribution<double> offset_dist{0.01, 5.0};
    std::uniform_int_distribution<uint64_t> qty_dist{1, 100};
    std::uniform_int_distribution<int> op_dist{0, 99};
    uint64_t next_id{1};

    void reset() {
        rng.seed(RANDOM_SEED);
        next_id = 1;
    }

    double generatePrice(Order::Side side) {
        double offset = offset_dist(rng);
        return (side == Order::Side::BUY) ? (MID_PRICE - offset) : (MID_PRICE + offset);
    }

    Order generateLimitOrder() {
        Order::Side side = (side_dist(rng) == 0) ? Order::Side::BUY : Order::Side::SELL;
        double price = generatePrice(side);
        uint64_t qty = qty_dist(rng);
        return Order{next_id++, side, price, qty, Order::OrderType::LIMIT};
    }

    Order generateMarketOrder() {
        Order::Side side = (side_dist(rng) == 0) ? Order::Side::BUY : Order::Side::SELL;
        uint64_t qty = qty_dist(rng);
        // Market orders ignore price during matching; 0.0 is passed as placeholder
        return Order{next_id++, side, 0.0, qty, Order::OrderType::MARKET};
    }
};

enum class OpType { SUBMIT, CANCEL, MODIFY };

struct MixedOp {
    OpType type;
    Order order;       // Used if SUBMIT
    size_t id_index;   // Target index into active IDs vector if CANCEL or MODIFY
    double new_price;  // Used if MODIFY
    uint64_t new_qty;  // Used if MODIFY
};

struct LatencySample {
    size_t order_index;
    double latency_us;
    size_t book_size;
};

static std::vector<MixedOp> generateMixedWorkload(OrderGenerator& gen, size_t count) {
    std::vector<MixedOp> ops;
    ops.reserve(count);
    std::uniform_int_distribution<size_t> idx_dist(0, 1000000);

    for (size_t i = 0; i < count; ++i) {
        int roll = gen.op_dist(gen.rng);
        if (roll < 70) {
            // 70% submitOrder
            ops.push_back({OpType::SUBMIT, gen.generateLimitOrder(), 0, 0.0, 0});
        } else if (roll < 90) {
            // 20% cancelOrder
            ops.push_back({OpType::CANCEL, Order{0, Order::Side::BUY, 0.0, 0}, idx_dist(gen.rng), 0.0, 0});
        } else {
            // 10% modifyOrder
            Order::Side side = (gen.side_dist(gen.rng) == 0) ? Order::Side::BUY : Order::Side::SELL;
            double new_price = gen.generatePrice(side);
            uint64_t new_qty = gen.qty_dist(gen.rng);
            ops.push_back({OpType::MODIFY, Order{0, Order::Side::BUY, 0.0, 0}, idx_dist(gen.rng), new_price, new_qty});
        }
    }
    return ops;
}

static double getPercentile(const std::vector<double>& sorted_samples, double pct) {
    if (sorted_samples.empty()) return 0.0;
    size_t idx = static_cast<size_t>(pct * sorted_samples.size());
    if (idx >= sorted_samples.size()) idx = sorted_samples.size() - 1;
    return sorted_samples[idx];
}

int main() {
    // TASK 1: Empirical Clock Resolution Diagnostic
    using ClockPeriod = std::chrono::steady_clock::period;
    double ns_per_tick = (static_cast<double>(ClockPeriod::num) / static_cast<double>(ClockPeriod::den)) * 1e9;
    std::cout << "steady_clock period: " << ClockPeriod::num << "/" << ClockPeriod::den
              << " (" << ns_per_tick << " ns declared type period (not actual hardware resolution))\n";

    // Empirically measure smallest NONZERO tick difference across tight iterations
    uint64_t min_nonzero_ns = UINT64_MAX;
    for (int i = 0; i < 100000; ++i) {
        auto t1 = std::chrono::steady_clock::now();
        auto t2 = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        if (diff > 0 && static_cast<uint64_t>(diff) < min_nonzero_ns) {
            min_nonzero_ns = static_cast<uint64_t>(diff);
        }
    }
    if (min_nonzero_ns != UINT64_MAX) {
        std::cout << "Empirically measured smallest resolvable duration: " << min_nonzero_ns << " nanoseconds\n\n";
    } else {
        std::cout << "Empirically measured smallest resolvable duration: < 1 ns (0 ns observed across all samples)\n\n";
    }

    OrderGenerator generator;

    // =========================================================================
    // Scenario (b)(i): addOrder-only (100,000 LIMIT orders)
    // =========================================================================
    constexpr size_t ADD_ORDER_COUNT = 100000;
    constexpr size_t WARMUP_COUNT = 1000;

    generator.reset();
    std::vector<Order> warmup_add_orders;
    warmup_add_orders.reserve(WARMUP_COUNT);
    for (size_t i = 0; i < WARMUP_COUNT; ++i) {
        warmup_add_orders.push_back(generator.generateLimitOrder());
    }

    std::vector<Order> add_orders;
    add_orders.reserve(ADD_ORDER_COUNT);
    for (size_t i = 0; i < ADD_ORDER_COUNT; ++i) {
        add_orders.push_back(generator.generateLimitOrder());
    }

    OrderBook book_add;
    // Warmup (untimed)
    for (const auto& ord : warmup_add_orders) {
        book_add.addOrder(ord);
    }

    // Timed batch
    auto start_add = std::chrono::steady_clock::now();
    for (const auto& ord : add_orders) {
        book_add.addOrder(ord);
    }
    auto end_add = std::chrono::steady_clock::now();

    double sec_add = std::chrono::duration<double>(end_add - start_add).count();
    double throughput_add = static_cast<double>(ADD_ORDER_COUNT) / sec_add;


    // =========================================================================
    // Scenario (b)(ii) & (c): submitOrder with matching (100,000 LIMIT orders)
    // =========================================================================
    constexpr size_t SUBMIT_ORDER_COUNT = 100000;

    generator.reset();
    std::vector<Order> warmup_submit_orders;
    warmup_submit_orders.reserve(WARMUP_COUNT);
    for (size_t i = 0; i < WARMUP_COUNT; ++i) {
        warmup_submit_orders.push_back(generator.generateLimitOrder());
    }

    std::vector<Order> submit_orders;
    submit_orders.reserve(SUBMIT_ORDER_COUNT);
    for (size_t i = 0; i < SUBMIT_ORDER_COUNT; ++i) {
        submit_orders.push_back(generator.generateLimitOrder());
    }

    OrderBook book_submit;
    // Warmup (untimed)
    for (const auto& ord : warmup_submit_orders) {
        book_submit.submitOrder(ord);
    }

    std::vector<LatencySample> submit_samples;
    submit_samples.reserve(SUBMIT_ORDER_COUNT);

    auto start_submit = std::chrono::steady_clock::now();
    for (size_t i = 0; i < SUBMIT_ORDER_COUNT; ++i) {
        size_t current_book_size = book_submit.totalOrderCount();
        auto t1 = std::chrono::steady_clock::now();
        book_submit.submitOrder(submit_orders[i]);
        auto t2 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t2 - t1).count();
        submit_samples.push_back({i, us, current_book_size});
    }
    auto end_submit = std::chrono::steady_clock::now();

    double sec_submit = std::chrono::duration<double>(end_submit - start_submit).count();
    double throughput_submit = static_cast<double>(SUBMIT_ORDER_COUNT) / sec_submit;

    // Extract latency values for sorting percentiles
    std::vector<double> submit_latencies_us;
    submit_latencies_us.reserve(SUBMIT_ORDER_COUNT);
    for (const auto& sample : submit_samples) {
        submit_latencies_us.push_back(sample.latency_us);
    }

    std::sort(submit_latencies_us.begin(), submit_latencies_us.end());
    double submit_p50  = getPercentile(submit_latencies_us, 0.50);
    double submit_p95  = getPercentile(submit_latencies_us, 0.95);
    double submit_p99  = getPercentile(submit_latencies_us, 0.99);
    double submit_p999 = getPercentile(submit_latencies_us, 0.999);

    // TASK 2: Top 10 slowest submitOrder() calls analysis
    std::vector<LatencySample> sorted_samples_by_latency = submit_samples;
    std::sort(sorted_samples_by_latency.begin(), sorted_samples_by_latency.end(),
              [](const LatencySample& a, const LatencySample& b) {
                  return a.latency_us > b.latency_us; // Descending
              });


    // =========================================================================
    // Scenario (b)(iii): Mixed Workload (70/20/10, 100,000 operations)
    // =========================================================================
    constexpr size_t MIXED_OP_COUNT = 100000;

    generator.reset();
    auto warmup_mixed_ops = generateMixedWorkload(generator, WARMUP_COUNT);
    auto mixed_ops = generateMixedWorkload(generator, MIXED_OP_COUNT);

    OrderBook book_mixed;
    std::vector<uint64_t> warmup_active_ids;
    warmup_active_ids.reserve(WARMUP_COUNT);

    // Warmup (untimed)
    for (const auto& op : warmup_mixed_ops) {
        if (op.type == OpType::SUBMIT) {
            book_mixed.submitOrder(op.order);
            warmup_active_ids.push_back(op.order.id);
        } else if (op.type == OpType::CANCEL && !warmup_active_ids.empty()) {
            uint64_t target_id = warmup_active_ids[op.id_index % warmup_active_ids.size()];
            book_mixed.cancelOrder(target_id);
        } else if (op.type == OpType::MODIFY && !warmup_active_ids.empty()) {
            uint64_t target_id = warmup_active_ids[op.id_index % warmup_active_ids.size()];
            book_mixed.modifyOrder(target_id, op.new_price, op.new_qty);
        }
    }

    OrderBook book_mixed_timed;
    std::vector<uint64_t> active_ids;
    active_ids.reserve(MIXED_OP_COUNT);

    size_t successful_ops = 0;

    // Timed run
    // RIGOR NOTE:
    // Every operation in mixed_ops is executed and timed. If cancelOrder or modifyOrder
    // targets an order ID that has already been consumed by a prior match fill, the method
    // gracefully returns false / std::nullopt. We count throughput as total operations
    // attempted per second (MIXED_OP_COUNT / sec) because every attempt performs full hash-map
    // index lookup work. We also track successful_ops for precise accounting.
    auto start_mixed = std::chrono::steady_clock::now();
    for (const auto& op : mixed_ops) {
        if (op.type == OpType::SUBMIT) {
            book_mixed_timed.submitOrder(op.order);
            active_ids.push_back(op.order.id);
            successful_ops++;
        } else if (op.type == OpType::CANCEL) {
            if (!active_ids.empty()) {
                uint64_t target_id = active_ids[op.id_index % active_ids.size()];
                if (book_mixed_timed.cancelOrder(target_id)) {
                    successful_ops++;
                }
            }
        } else if (op.type == OpType::MODIFY) {
            if (!active_ids.empty()) {
                uint64_t target_id = active_ids[op.id_index % active_ids.size()];
                auto res = book_mixed_timed.modifyOrder(target_id, op.new_price, op.new_qty);
                if (res.has_value()) {
                    successful_ops++;
                }
            }
        }
    }
    auto end_mixed = std::chrono::steady_clock::now();

    double sec_mixed = std::chrono::duration<double>(end_mixed - start_mixed).count();
    double throughput_mixed = static_cast<double>(MIXED_OP_COUNT) / sec_mixed;


    // =========================================================================
    // Scenario (d): Market Order Benchmark (10,000 MARKET orders)
    // =========================================================================
    constexpr size_t MARKET_ORDER_COUNT = 10000;
    constexpr size_t PREPOPULATE_LIMIT_COUNT = 20000;

    generator.reset();
    OrderBook book_market;

    // Pre-populate book with resting LIMIT orders so market orders have depth to match
    for (size_t i = 0; i < PREPOPULATE_LIMIT_COUNT; ++i) {
        Order ord = generator.generateLimitOrder();
        ord.quantity = 500; // Deep liquidity
        book_market.addOrder(ord);
    }

    std::vector<Order> warmup_market_orders;
    warmup_market_orders.reserve(WARMUP_COUNT);
    for (size_t i = 0; i < WARMUP_COUNT; ++i) {
        warmup_market_orders.push_back(generator.generateMarketOrder());
    }

    std::vector<Order> market_orders;
    market_orders.reserve(MARKET_ORDER_COUNT);
    for (size_t i = 0; i < MARKET_ORDER_COUNT; ++i) {
        market_orders.push_back(generator.generateMarketOrder());
    }

    // Warmup (untimed)
    for (const auto& ord : warmup_market_orders) {
        book_market.submitOrder(ord);
    }

    std::vector<double> market_latencies_us;
    market_latencies_us.reserve(MARKET_ORDER_COUNT);

    auto start_market = std::chrono::steady_clock::now();
    for (const auto& ord : market_orders) {
        auto t1 = std::chrono::steady_clock::now();
        book_market.submitOrder(ord);
        auto t2 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t2 - t1).count();
        market_latencies_us.push_back(us);
    }
    auto end_market = std::chrono::steady_clock::now();

    double sec_market = std::chrono::duration<double>(end_market - start_market).count();
    double throughput_market = static_cast<double>(MARKET_ORDER_COUNT) / sec_market;

    std::sort(market_latencies_us.begin(), market_latencies_us.end());
    double market_p50  = getPercentile(market_latencies_us, 0.50);
    double market_p95  = getPercentile(market_latencies_us, 0.95);
    double market_p99  = getPercentile(market_latencies_us, 0.99);
    double market_p999 = getPercentile(market_latencies_us, 0.999);


    // =========================================================================
    // Summary Table Output (Hygiene: strictly outside all timed regions)
    // =========================================================================
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "\n====================================================================================================\n";
    std::cout << "                                LEVIATHAN MATCHING ENGINE BENCHMARK                                \n";
    std::cout << "====================================================================================================\n\n";

    std::cout << "THROUGHPUT BENCHMARKS:\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(38) << "Scenario"
              << std::right << std::setw(15) << "Operations"
              << std::setw(18) << "Total Time (s)"
              << std::setw(24) << "Throughput (ops/sec)" << "\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";

    std::cout << std::left << std::setw(38) << "(b)(i)   addOrder (Limit, no match)"
              << std::right << std::setw(15) << ADD_ORDER_COUNT
              << std::setw(18) << sec_add
              << std::setw(24) << std::setprecision(2) << throughput_add << "\n";

    std::cout << std::left << std::setw(38) << "(b)(ii)  submitOrder (Limit + Match)"
              << std::right << std::setw(15) << SUBMIT_ORDER_COUNT
              << std::setw(18) << std::setprecision(6) << sec_submit
              << std::setw(24) << std::setprecision(2) << throughput_submit << "\n";

    std::cout << std::left << std::setw(38) << "(b)(iii) Mixed Workload (70/20/10)"
              << std::right << std::setw(15) << MIXED_OP_COUNT
              << std::setw(18) << std::setprecision(6) << sec_mixed
              << std::setw(24) << std::setprecision(2) << throughput_mixed << "\n";

    std::cout << std::left << std::setw(38) << "(d)      Market Order (Pre-populated)"
              << std::right << std::setw(15) << MARKET_ORDER_COUNT
              << std::setw(18) << std::setprecision(6) << sec_market
              << std::setw(24) << std::setprecision(2) << throughput_market << "\n";

    std::cout << "----------------------------------------------------------------------------------------------------\n";
    std::cout << "  * Mixed workload details: " << successful_ops << " / " << MIXED_OP_COUNT << " operations succeeded.\n\n";

    std::cout << "LATENCY DISTRIBUTION (Microseconds / us):\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(18) << "Percentile"
              << std::right << std::setw(35) << "(c) submitOrder (Limit + Match)"
              << std::setw(42) << "(d) Market Orders (Pre-populated)" << "\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(18) << "p50"
              << std::right << std::setw(35) << std::setprecision(4) << submit_p50
              << std::setw(42) << std::setprecision(4) << market_p50 << "\n";
    std::cout << std::left << std::setw(18) << "p95"
              << std::right << std::setw(35) << std::setprecision(4) << submit_p95
              << std::setw(42) << std::setprecision(4) << market_p95 << "\n";
    std::cout << std::left << std::setw(18) << "p99"
              << std::right << std::setw(35) << std::setprecision(4) << submit_p99
              << std::setw(42) << std::setprecision(4) << market_p99 << "\n";
    std::cout << std::left << std::setw(18) << "p99.9"
              << std::right << std::setw(35) << std::setprecision(4) << submit_p999
              << std::setw(42) << std::setprecision(4) << market_p999 << "\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n\n";

    std::cout << "TOP 10 SLOWEST submitOrder() CALLS IN SCENARIO (c):\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(10) << "Rank"
              << std::right << std::setw(16) << "Order Index"
              << std::setw(20) << "Latency (us)"
              << std::setw(24) << "Book Size (orders)" << "\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    for (size_t i = 0; i < std::min<size_t>(10, sorted_samples_by_latency.size()); ++i) {
        std::cout << std::left << std::setw(10) << (i + 1)
                  << std::right << std::setw(16) << sorted_samples_by_latency[i].order_index
                  << std::setw(20) << std::setprecision(4) << sorted_samples_by_latency[i].latency_us
                  << std::setw(24) << sorted_samples_by_latency[i].book_size << "\n";
    }
    std::cout << "====================================================================================================\n\n";

    return 0;
}
