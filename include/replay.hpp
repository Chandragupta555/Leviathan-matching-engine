#pragma once

#include "order_book.hpp"

#include <istream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ReplayResult — holds everything produced by replaying a log file.
// ---------------------------------------------------------------------------
struct ReplayResult {
    OrderBook            book;              // Final book state after replay
    std::vector<Trade>   replayed_trades;   // Trades produced by replay operations
    std::vector<Trade>   logged_trades;     // TRADE lines parsed from the log file
    size_t               mismatches{0};     // Count of trade comparison failures
    std::string          mismatch_report;   // Human-readable mismatch details
};

// ---------------------------------------------------------------------------
// Core replay functions — factored out so both replay_tool and tests can
// call them without duplicating parsing logic.
//
// Trade comparison ignores the timestamp field because replayed Orders get
// new timestamps from Order's global counter.  See event_log.hpp for the
// full rationale.
// ---------------------------------------------------------------------------

// Replay from an already-open input stream (for testing with istringstream).
ReplayResult replayFromStream(std::istream& input);

// Replay from a file path (for the CLI tool).
ReplayResult replayFromFile(const std::string& filepath);
