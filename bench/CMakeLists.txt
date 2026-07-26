find_package(benchmark REQUIRED)
find_package(Threads REQUIRED)

add_executable(privateer_hash_bench hash_bench.cpp)
target_link_libraries(privateer_hash_bench PRIVATE privateer::privateer benchmark::benchmark)

add_executable(privateer_region_bench region_bench.cpp)
target_include_directories(privateer_region_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test)
target_link_libraries(privateer_region_bench PRIVATE privateer::privateer benchmark::benchmark Threads::Threads)
