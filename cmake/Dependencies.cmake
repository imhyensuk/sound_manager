include(FetchContent)

# CMake 4 refuses projects that declare compatibility with CMake < 3.5 (doctest 2.4.11 does).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

if(SMIX_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(doctest)
endif()

# Local language model runtime (requirement: fully offline). Built static, CPU portable,
# without any networking (no curl / OpenSSL), without tools, tests or examples.
option(SMIX_WITH_LLAMA "Build the local language model (llama.cpp)" ON)
set(SMIX_LLAMA_TAG "b11121" CACHE STRING "llama.cpp release tag")
if(SMIX_WITH_LLAMA)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
    set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
    set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)     # portable binaries for users' machines
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)     # no OpenMP runtime inside a host process
    set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG ${SMIX_LLAMA_TAG}
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(llama)
endif()

if(SMIX_BUILD_PLUGIN)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()
