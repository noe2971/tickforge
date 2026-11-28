include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(tickforge_tests
    test_placeholder.cpp
    test_itch_endian.cpp
    test_itch_decode.cpp
    test_itch_stream.cpp
    test_gzip_source.cpp
    test_locate_table.cpp
    test_symbol_filter.cpp
    test_compact_file.cpp
    test_message_counter.cpp
    test_book_containers.cpp
    test_book_engines.cpp
    test_book_fuzz.cpp
    test_replay_validator.cpp
    test_top_levels.cpp
    test_queue_jumps.cpp
)

target_compile_options(tickforge_tests PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(tickforge_tests PRIVATE
    tickforge_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(tickforge_tests)

# Replaces global operator new to count allocations, so it gets its own
# program instead of affecting every other test.
add_executable(tickforge_alloc_tests test_book_alloc.cpp)
target_compile_options(tickforge_alloc_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tickforge_alloc_tests PRIVATE tickforge_core GTest::gtest_main)
gtest_discover_tests(tickforge_alloc_tests)
