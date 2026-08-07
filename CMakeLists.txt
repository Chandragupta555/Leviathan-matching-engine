cmake_minimum_required(VERSION 3.15)
project(Leviathan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Core library (static) ---
add_library(order_book STATIC
    src/order_book.cpp
)
target_include_directories(order_book PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# --- Test executable ---
enable_testing()

add_executable(test_order_book
    tests/test_order_book.cpp
)
target_link_libraries(test_order_book PRIVATE order_book)

add_test(NAME OrderBookTests COMMAND test_order_book)

add_executable(test_matching
    tests/test_matching.cpp
)
target_link_libraries(test_matching PRIVATE order_book)

add_test(NAME MatchingTests COMMAND test_matching)

add_executable(test_market_orders
    tests/test_market_orders.cpp
)
target_link_libraries(test_market_orders PRIVATE order_book)

add_test(NAME MarketOrderTests COMMAND test_market_orders)

add_executable(test_modify_order
    tests/test_modify_order.cpp
)
target_link_libraries(test_modify_order PRIVATE order_book)

add_test(NAME ModifyOrderTests COMMAND test_modify_order)

# --- Benchmark executable ---
add_executable(bench_order_book
    benchmarks/bench_order_book.cpp
)
target_link_libraries(bench_order_book PRIVATE order_book)

# --- Recording / Replay library (static) ---
add_library(recording_replay STATIC
    src/recording_order_book.cpp
    src/replay.cpp
)
target_link_libraries(recording_replay PUBLIC order_book)

# --- Replay tool executable ---
add_executable(replay_tool
    tools/replay_tool.cpp
)
target_link_libraries(replay_tool PRIVATE recording_replay)

# --- Replay tests ---
add_executable(test_replay
    tests/test_replay.cpp
)
target_link_libraries(test_replay PRIVATE recording_replay)

add_test(NAME ReplayTests COMMAND test_replay)
