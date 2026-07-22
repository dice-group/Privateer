find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

add_executable(privateer_smoke_test smoke_test.cpp)
target_link_libraries(privateer_smoke_test PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_smoke_test)

# platform probes: validate the design assumptions the engine is built on
# (architecture section 14); no library code, plain platform APIs
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
