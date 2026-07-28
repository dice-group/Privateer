cmake_minimum_required(VERSION 3.24)
project(privateer VERSION 0.2.0 LANGUAGES CXX)

option(PRIVATEER_BUILD_TESTING "Build the test suites" ${PROJECT_IS_TOP_LEVEL})
option(PRIVATEER_BUILD_BENCHMARKS "Build the microbenchmarks" ${PROJECT_IS_TOP_LEVEL})

find_package(asio REQUIRED)
find_package(Boost REQUIRED)
find_package(xxHash REQUIRED)
find_package(Threads REQUIRED)

add_library(privateer STATIC
        src/version.cpp
        src/block_hash.cpp
        src/block_store.cpp
        src/error.cpp
        src/fault_handler.cpp
        src/file_util.cpp
        src/logger.cpp
        src/mlocked.cpp
        src/executor.cpp
        src/recipe.cpp
        src/region.cpp
        src/region_registry.cpp
        src/resident.cpp
        src/rlimits.cpp
        src/slot_table.cpp
        src/vm.cpp
        src/word_wait.cpp
        src/asio_src.cpp
)
add_library(privateer::privateer ALIAS privateer)

target_compile_features(privateer PUBLIC cxx_std_23)
set_target_properties(privateer PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(privateer PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# asio configuration: standalone, one implementation TU (src/asio_src.cpp), no deprecated APIs.
# PUBLIC because engine headers will include asio headers and consumers must agree on these.
target_compile_definitions(privateer PUBLIC
        ASIO_STANDALONE
        ASIO_SEPARATE_COMPILATION
        ASIO_NO_DEPRECATED
)
target_compile_definitions(privateer PRIVATE PRIVATEER_VERSION="${PROJECT_VERSION}")
target_link_libraries(privateer PUBLIC asio::asio Boost::headers Threads::Threads)
target_link_libraries(privateer PRIVATE xxHash::xxhash)

if (PRIVATEER_BUILD_TESTING)
    # Compile the test-only hooks into the library (failure-injection seams,
    # crash-test kill points, internal accessors). PUBLIC so the library and
    # every test executable agree on the declarations; excluded from consumer
    # builds, which do not build the tests.
    target_compile_definitions(privateer PUBLIC PRIVATEER_TEST_HOOKS=1)

    enable_testing()
    add_subdirectory(test)
endif ()

if (PRIVATEER_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif ()

include(GNUInstallDirs)
install(TARGETS privateer EXPORT privateer-targets)
install(DIRECTORY include/privateer/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/privateer)
install(EXPORT privateer-targets NAMESPACE privateer:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/privateer)
