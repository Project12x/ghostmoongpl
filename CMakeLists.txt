cmake_minimum_required(VERSION 3.22)
project(ghostmoon_oss LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# ghostmoon-oss: the distributable open-source slice of Ghostmoon that GPL
# plugins (Nessy, Breadbin, Pedalboard3) can legally link. Mixed per-file
# licensing: first-party shared code is MIT; copyleft-derived DSP (ReverbSC,
# Csound-derived) is LGPL-2.1-or-later. Every file carries an SPDX header.
# See LICENSES/ and README.md.
# ---------------------------------------------------------------------------

# DSP (header-only): MIT utils (future) + LGPL ReverbSC family.
add_library(ghostmoon_oss_dsp INTERFACE)
target_include_directories(ghostmoon_oss_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/dsp/include")
target_compile_features(ghostmoon_oss_dsp INTERFACE cxx_std_20)
add_library(ghostmoon_oss::dsp ALIAS ghostmoon_oss_dsp)

# Core (header-only, MIT): skin-neutral UI scaffolding (Geometry, ScaledEditor).
# Geometry is JUCE-free; ScaledEditor needs JUCE provided by the consumer.
add_library(ghostmoon_oss_core INTERFACE)
target_include_directories(ghostmoon_oss_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
target_compile_features(ghostmoon_oss_core INTERFACE cxx_std_20)
add_library(ghostmoon_oss::core ALIAS ghostmoon_oss_core)

# Synthwave UI skin (header-only, MIT): Breadbin's neon draws. Depends on core.
add_library(ghostmoon_oss_ui_synthwave INTERFACE)
target_include_directories(ghostmoon_oss_ui_synthwave INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
target_compile_features(ghostmoon_oss_ui_synthwave INTERFACE cxx_std_20)
target_link_libraries(ghostmoon_oss_ui_synthwave INTERFACE ghostmoon_oss_core)
add_library(ghostmoon_oss::ui_synthwave ALIAS ghostmoon_oss_ui_synthwave)

option(GHOSTMOON_OSS_BUILD_TESTS "Build ghostmoon-oss tests (GoogleTest)" OFF)
if(GHOSTMOON_OSS_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)

    add_executable(ghostmoon_oss_dsp_tests tests/test_reverbscx.cpp)
    target_link_libraries(ghostmoon_oss_dsp_tests PRIVATE ghostmoon_oss_dsp GTest::gtest_main)
    gtest_discover_tests(ghostmoon_oss_dsp_tests)

    add_executable(ghostmoon_oss_geometry_tests tests/test_ui_geometry.cpp)
    target_link_libraries(ghostmoon_oss_geometry_tests PRIVATE ghostmoon_oss_core GTest::gtest_main)
    gtest_discover_tests(ghostmoon_oss_geometry_tests)
endif()
