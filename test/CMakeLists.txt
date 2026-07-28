find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

# PRE_TEST: ctest discovers tests serially at startup. The POST_BUILD default
# runs one discovery step per target during the build; those steps share one
# temp file per directory (CMake 4.x names it by an empty-string hash) and
# race under parallel ninja when several test binaries link at once.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

# Wall-clock ceiling for every test. The suites run in seconds, so this only
# ever fires on a deadlock, and it must fire: a hung test process holds its
# ctest slot forever, and a hang in library teardown is past every check the
# test itself could make. Suites that legitimately run longer raise it on
# their own tests.
set(PRIVATEER_TEST_TIMEOUT 300)

add_executable(privateer_smoke_test smoke_test.cpp)
target_link_libraries(privateer_smoke_test PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_smoke_test PROPERTIES TIMEOUT ${PRIVATEER_TEST_TIMEOUT})

add_executable(privateer_unit_tests
        block_hash_tests.cpp
        block_store_tests.cpp
        commit_crash_tests.cpp
        error_tests.cpp
        file_util_tests.cpp
        file_util_crash_tests.cpp
        logger_tests.cpp
        recipe_tests.cpp
        region_cleaner_tests.cpp
        region_commit_crash_tests.cpp
        region_commit_tests.cpp
        region_executor_tests.cpp
        region_free_tests.cpp
        region_governor_tests.cpp
        region_snapshot_tests.cpp
        region_tests.cpp
        region_write_tests.cpp
        slot_table_tests.cpp
        support/sanitizer_options.cpp
        vm_tests.cpp
        word_wait_tests.cpp)
target_include_directories(privateer_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_unit_tests PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_unit_tests PROPERTIES TIMEOUT ${PRIVATEER_TEST_TIMEOUT})

# own binary: these tests own the process-wide fault-signal dispositions
add_executable(privateer_barrier_tests
        registry_tests.cpp
        fault_handler_tests.cpp
        memlock_tests.cpp
        support/sanitizer_options.cpp)
target_include_directories(privateer_barrier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_barrier_tests PRIVATE privateer::privateer GTest::gtest_main Threads::Threads)
gtest_discover_tests(privateer_barrier_tests PROPERTIES TIMEOUT ${PRIVATEER_TEST_TIMEOUT})

# own binary: the combined long-running soak; excluded from CI by its label,
# run locally with ctest -L long_running (PRIVATEER_SOAK_SECONDS stretches it)
add_executable(privateer_soak_tests region_soak_tests.cpp)
target_include_directories(privateer_soak_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_soak_tests PRIVATE privateer::privateer GTest::gtest_main Threads::Threads)
gtest_discover_tests(privateer_soak_tests PROPERTIES LABELS long_running TIMEOUT 900)

# own binary: its strong metall_log must replace the weak built-in sink at link time
add_executable(privateer_logger_override_test logger_override_tests.cpp)
target_include_directories(privateer_logger_override_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_logger_override_test PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_logger_override_test PROPERTIES TIMEOUT ${PRIVATEER_TEST_TIMEOUT})

# platform probes: validate the platform assumptions the engine is built on;
# no library code, plain platform APIs
add_executable(privateer_probes
        probes/barrier_probe.cpp
        probes/shootdown_probe.cpp
        probes/remap_probe.cpp
        probes/durability_probe.cpp)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(privateer_probes PRIVATE probes/residency_probe.cpp)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        target_sources(privateer_probes PRIVATE probes/tbi_probe.cpp)
    endif ()
endif ()
target_compile_features(privateer_probes PRIVATE cxx_std_23)
target_include_directories(privateer_probes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_probes PRIVATE GTest::gtest_main Threads::Threads)
gtest_discover_tests(privateer_probes PROPERTIES LABELS probes TIMEOUT ${PRIVATEER_TEST_TIMEOUT})
