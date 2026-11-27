cmake_minimum_required(VERSION 3.21)

project(tickforge
    VERSION 0.1.0
    DESCRIPTION "C++20 limit order book engine and queue-aware market-making simulator for Nasdaq TotalView-ITCH 5.0"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(TICKFORGE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TICKFORGE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(TICKFORGE_BUILD_TESTS "Build tests" ON)
option(TICKFORGE_BUILD_BENCH "Build benchmarks" ON)
option(TICKFORGE_BUILD_TOOLS "Build command-line tools (ITCH count/filter)" ON)

find_package(ZLIB REQUIRED)

add_library(tickforge_core
    src/placeholder.cpp
    src/io/byte_source.cpp
    src/itch/compact_file.cpp
    src/itch/message_counter.cpp
    src/book/replay_validator.cpp
    src/book/top_levels.cpp
)

# Position-independent so the static library can be linked into the Python
# extension module.
set_target_properties(tickforge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(tickforge_core PUBLIC ZLIB::ZLIB)

target_include_directories(tickforge_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_options(tickforge_core PRIVATE
    -Wall -Wextra -Wpedantic
)

if(TICKFORGE_ENABLE_ASAN)
    target_compile_options(tickforge_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(tickforge_core PUBLIC -fsanitize=address)
endif()

if(TICKFORGE_ENABLE_UBSAN)
    target_compile_options(tickforge_core PUBLIC -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(tickforge_core PUBLIC -fsanitize=undefined)
endif()

if(TICKFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TICKFORGE_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(TICKFORGE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
