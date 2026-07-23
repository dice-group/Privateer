cmake_minimum_required(VERSION 3.24)
project(privateer VERSION 0.2.0 LANGUAGES CXX)

option(PRIVATEER_BUILD_TESTING "Build the test suites" ${PROJECT_IS_TOP_LEVEL})
option(PRIVATEER_BUILD_LEGACY "Build the inherited v0.1 engine (Linux x86_64 only)" OFF)

find_package(asio REQUIRED)
find_package(Threads REQUIRED)

add_library(privateer STATIC
        src/version.cpp
        src/error.cpp
        src/logger.cpp
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
target_link_libraries(privateer PUBLIC asio::asio Threads::Threads)

if (PRIVATEER_BUILD_LEGACY)
    if (NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
        message(FATAL_ERROR "The legacy engine decodes faults via the x86_64 signal context and builds on Linux x86_64 only.")
    endif ()

    find_package(OpenSSL REQUIRED)
    find_package(OpenMP REQUIRED)
    find_package(Boost REQUIRED)
    find_package(spdlog REQUIRED)

    add_library(privateer-legacy STATIC
            src/privateer/block_storage_base.cpp
            src/privateer/block_storage_factory.cpp
            src/privateer/posix_block_storage.cpp
            src/privateer/sigaction_virtual_memory_manager.cpp
            src/privateer/smartcache_block_storage.cpp
            src/privateer/uffd_virtual_memory_manager.cpp
            src/privateer/virtual_memory_manager_base.cpp
            src/privateer/virtual_memory_manager_factory.cpp
            src/privateer_c.cpp
    )
    target_compile_features(privateer-legacy PUBLIC cxx_std_17)
    target_include_directories(privateer-legacy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(privateer-legacy PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_ERROR)
    target_link_libraries(privateer-legacy PUBLIC
            OpenSSL::SSL OpenSSL::Crypto
            OpenMP::OpenMP_CXX
            Boost::headers
            spdlog::spdlog
            Threads::Threads
            rt
    )
endif ()

if (PRIVATEER_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif ()

include(GNUInstallDirs)
install(TARGETS privateer EXPORT privateer-targets)
install(DIRECTORY include/privateer/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/privateer)
install(EXPORT privateer-targets NAMESPACE privateer:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/privateer)
