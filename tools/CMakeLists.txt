add_executable(itch_count itch_count.cpp)
target_link_libraries(itch_count PRIVATE tickforge_core)
target_compile_options(itch_count PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_filter itch_filter.cpp)
target_link_libraries(itch_filter PRIVATE tickforge_core)
target_compile_options(itch_filter PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
add_executable(book_fuzz book_fuzz.cpp)
target_link_libraries(book_fuzz PRIVATE tickforge_core Threads::Threads)
target_compile_options(book_fuzz PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_validate itch_validate.cpp)
target_link_libraries(itch_validate PRIVATE tickforge_core)
target_compile_options(itch_validate PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_engine_diff itch_engine_diff.cpp)
target_link_libraries(itch_engine_diff PRIVATE tickforge_core)
target_compile_options(itch_engine_diff PRIVATE -Wall -Wextra -Wpedantic)

add_executable(itch_top_levels itch_top_levels.cpp)
target_link_libraries(itch_top_levels PRIVATE tickforge_core)
target_compile_options(itch_top_levels PRIVATE -Wall -Wextra -Wpedantic)
