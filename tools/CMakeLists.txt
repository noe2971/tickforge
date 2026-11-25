add_executable(itch_count itch_count.cpp)
target_link_libraries(itch_count PRIVATE tickforge_core)
target_compile_options(itch_count PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_filter itch_filter.cpp)
target_link_libraries(itch_filter PRIVATE tickforge_core)
target_compile_options(itch_filter PRIVATE -Wall -Wextra -Wpedantic)
