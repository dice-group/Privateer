cmake_minimum_required(VERSION 3.24)
project(PackageTest CXX)

find_package(privateer REQUIRED)

add_executable(example example.cpp)
target_link_libraries(example PRIVATE privateer::privateer)

set_target_properties(example PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
