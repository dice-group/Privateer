find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

# PRE_TEST: ctest discovers tests serially at startup. The POST_BUILD default
# runs one discovery step per target during the build; those steps share one
# temp file per directory (CMake 4.x names it by an empty-string hash) and
# race under parallel ninja when several test binaries link at once.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

add_executable(privateer_smoke_test smoke_test.cpp)
target_link_libraries(privateer_smoke_test PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_smoke_test)

add_executable(privateer_unit_tests
        block_hash_tests.cpp
        block_store_tests.cpp
        commit_crash_tests.cpp
        error_tests.cpp
        file_util_tests.cpp
        file_util_crash_tests.cpp
        logger_tests.cpp
        recipe_tests.cpp)
target_include_directories(privateer_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_unit_tests PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_unit_tests)

# own binary: these tests own the process-wide fault-signal dispositions
add_executable(privateer_barrier_tests
        registry_tests.cpp
        fault_handler_tests.cpp
        memlock_tests.cpp)
target_include_directories(privateer_barrier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_barrier_tests PRIVATE privateer::privateer GTest::gtest_main Threads::Threads)
gtest_discover_tests(privateer_barrier_tests)

# own binary: its strong metall_log must replace the weak built-in sink at link time
add_executable(privateer_logger_override_test logger_override_tests.cpp)
target_include_directories(privateer_logger_override_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(privateer_logger_override_test PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_logger_override_test)

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
gtest_discover_tests(privateer_probes PROPERTIES LABELS probes)

if (PRIVATEER_BUILD_LEGACY)
    add_executable(privateer_legacy_test PrivateerTest.cpp)
    target_link_libraries(privateer_legacy_test PRIVATE privateer-legacy GTest::gtest_main)
    gtest_discover_tests(privateer_legacy_test PROPERTIES LABELS legacy)
endif ()
