cmake_minimum_required(VERSION 3.22)
project(SoundManager VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SMIX_BUILD_TESTS  "Build core unit tests" ON)
option(SMIX_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/AAX/Standalone)" ON)

include(cmake/Dependencies.cmake)

add_subdirectory(core)

if(SMIX_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
