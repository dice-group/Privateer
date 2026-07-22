find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(privateer_smoke_test smoke_test.cpp)
target_link_libraries(privateer_smoke_test PRIVATE privateer::privateer GTest::gtest_main)
gtest_discover_tests(privateer_smoke_test)

if (PRIVATEER_BUILD_LEGACY)
    add_executable(privateer_legacy_test PrivateerTest.cpp)
    target_link_libraries(privateer_legacy_test PRIVATE privateer-legacy GTest::gtest_main)
    gtest_discover_tests(privateer_legacy_test PROPERTIES LABELS legacy)
endif ()
