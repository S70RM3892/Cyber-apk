cmake_minimum_required(VERSION 3.22)
project(cyber_apex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-independent core: builds on the host (asset tools, tests) and, later,
# with the Android NDK toolchain file for the runtime.
add_library(apex_core
    engine/src/specular_aa.cpp
    engine/src/cluster_cull.cpp
    engine/src/citygen.cpp
)
target_include_directories(apex_core PUBLIC engine/include)
if (MSVC)
    target_compile_options(apex_core PRIVATE /W4)
else()
    target_compile_options(apex_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(apex_tests tests/test_main.cpp)
    target_link_libraries(apex_tests PRIVATE apex_core)
    add_test(NAME apex_tests COMMAND apex_tests)
endif()
