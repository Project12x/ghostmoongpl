cmake_minimum_required(VERSION 3.22)
project(ghostmoongpl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# GhostmoonGPL: header-only, copyleft (LGPL-2.1-or-later) DSP components.
# Kept separate from the MIT Ghostmoon library so proprietary plugins never
# link copyleft code by accident. See LICENSE and README.md.
# ---------------------------------------------------------------------------
add_library(ghostmoongpl INTERFACE)
target_include_directories(ghostmoongpl INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/dsp/include")
target_compile_features(ghostmoongpl INTERFACE cxx_std_20)
add_library(ghostmoongpl::ghostmoongpl ALIAS ghostmoongpl)

# Header-only JUCE UI primitives (consumer must provide JUCE on the include path).
add_library(ghostmoongpl_ui INTERFACE)
target_include_directories(ghostmoongpl_ui INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
target_compile_features(ghostmoongpl_ui INTERFACE cxx_std_20)
add_library(ghostmoongpl::ui ALIAS ghostmoongpl_ui)

option(GHOSTMOONGPL_BUILD_TESTS "Build GhostmoonGPL reverb tests (GoogleTest)" OFF)
if(GHOSTMOONGPL_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(ghostmoongpl_tests tests/test_reverbscx.cpp)
    target_link_libraries(ghostmoongpl_tests PRIVATE ghostmoongpl GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(ghostmoongpl_tests)
endif()
