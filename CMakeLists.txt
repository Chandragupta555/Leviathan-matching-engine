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
