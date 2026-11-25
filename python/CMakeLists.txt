cmake_minimum_required(VERSION 3.21)
project(tickforge_python LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TICKFORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TICKFORGE_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(TICKFORGE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/tickforge_core_build)

find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(_tickforge src/bindings.cpp)
target_link_libraries(_tickforge PRIVATE tickforge_core)

install(TARGETS _tickforge DESTINATION tickforge)
