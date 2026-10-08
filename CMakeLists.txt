cmake_minimum_required(VERSION 3.22)
project(SoundManager VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sources contain UTF-8 (Korean UI text and chat vocabulary).
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(SMIX_BUILD_TESTS  "Build core unit tests" ON)
option(SMIX_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/AAX/Standalone)" ON)

include(cmake/Dependencies.cmake)
include(cmake/Memopro.cmake)

if(SMIX_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

if(SMIX_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
