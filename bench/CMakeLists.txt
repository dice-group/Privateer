find_package(benchmark REQUIRED)

add_executable(privateer_hash_bench hash_bench.cpp)
target_link_libraries(privateer_hash_bench PRIVATE privateer::privateer benchmark::benchmark)
