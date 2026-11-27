include(FetchContent)

FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(benchmark)

add_executable(tickforge_bench
    bench_placeholder.cpp
)

target_link_libraries(tickforge_bench PRIVATE
    tickforge_core
    benchmark::benchmark_main
)

# Full-day ITCH parse-throughput benchmark (standalone: one run over a
# multi-GB file doesn't fit Google Benchmark's repeat-a-small-loop model).
add_executable(itch_throughput itch_throughput.cpp)
target_link_libraries(itch_throughput PRIVATE tickforge_core)

# Full-day order book replay benchmarks: throughput, per-call latency and the
# fast-vs-reference speedup (PROGRESS.md, Rule E).
add_executable(book_replay book_replay.cpp)
target_link_libraries(book_replay PRIVATE tickforge_core)
target_compile_options(book_replay PRIVATE -Wall -Wextra -Wpedantic)
