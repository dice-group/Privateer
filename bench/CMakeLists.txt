# Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
# SPDX-License-Identifier: MIT

find_package(benchmark REQUIRED)
find_package(Threads REQUIRED)

add_executable(privateer_hash_bench hash_bench.cpp)
target_link_libraries(privateer_hash_bench PRIVATE privateer::privateer benchmark::benchmark)

add_executable(privateer_region_bench region_bench.cpp)
target_include_directories(privateer_region_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test)
target_link_libraries(privateer_region_bench PRIVATE privateer::privateer benchmark::benchmark Threads::Threads)

add_executable(privateer_addressing_bench addressing_bench.cpp)
target_include_directories(privateer_addressing_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test)
target_link_libraries(privateer_addressing_bench PRIVATE privateer::privateer benchmark::benchmark)

add_executable(privateer_recipe_bench recipe_bench.cpp)
target_include_directories(privateer_recipe_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test)
target_link_libraries(privateer_recipe_bench PRIVATE privateer::privateer benchmark::benchmark)

# The io_uring arms of the barrier benchmark. liburing is a benchmark-only
# dependency and stays out of the engine unless the batched barrier wins.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PRIVATEER_BENCH_URING_DEFAULT ON)
else ()
    set(PRIVATEER_BENCH_URING_DEFAULT OFF)
endif ()
option(PRIVATEER_BENCH_URING "Build the io_uring arms of the barrier benchmark (Linux)"
        ${PRIVATEER_BENCH_URING_DEFAULT})

add_executable(privateer_barrier_bench barrier_bench.cpp)
target_include_directories(privateer_barrier_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test)
target_link_libraries(privateer_barrier_bench PRIVATE privateer::privateer benchmark::benchmark Threads::Threads)
if (PRIVATEER_BENCH_URING)
    find_package(liburing REQUIRED)
    target_compile_definitions(privateer_barrier_bench PRIVATE PRIVATEER_BENCH_HAS_URING=1)
    target_link_libraries(privateer_barrier_bench PRIVATE liburing::liburing)
endif ()
