# memopro (https://github.com/imhyensuk/memopro): keeps Sound Manager's large buffers
# (knowledge index, history snapshots, hibernated plugin states, model weights, LLM state)
# under one hard memory budget, losslessly, without writing to disk.
#
# The Rust crate memopro-c is built with cargo and linked statically. Without cargo (or on
# Windows, which memopro does not support yet) a built-in fallback allocator with the same
# interface and budget accounting is used.

option(SMIX_WITH_MEMOPRO "Use the memopro runtime for large buffers" ON)
set(SMIX_MEMOPRO_SOURCE_DIR "" CACHE PATH "Local memopro checkout (default: fetched from GitHub)")
set(SMIX_MEMOPRO_GIT_TAG "a1934a6f7161305658ea267a94253cb096db8277" CACHE STRING "memopro commit")

set(SMIX_MEMOPRO_ENABLED OFF)
if(SMIX_WITH_MEMOPRO)
    find_program(SMIX_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
    if(WIN32)
        message(STATUS "memopro: Windows is not supported by memopro yet - using the fallback allocator")
    elseif(NOT SMIX_CARGO)
        message(WARNING "memopro: cargo not found - using the fallback allocator (install Rust to enable memopro)")
    else()
        if(SMIX_MEMOPRO_SOURCE_DIR)
            set(memopro_SOURCE_DIR "${SMIX_MEMOPRO_SOURCE_DIR}")
        else()
            FetchContent_Declare(memopro
                GIT_REPOSITORY https://github.com/imhyensuk/memopro.git
                GIT_TAG ${SMIX_MEMOPRO_GIT_TAG})
            FetchContent_MakeAvailable(memopro)  # no CMakeLists: just populates the sources
        endif()

        set(_mp_target_dir "${CMAKE_BINARY_DIR}/memopro-target")
        set(_mp_lib "${_mp_target_dir}/release/${CMAKE_STATIC_LIBRARY_PREFIX}memopro_c${CMAKE_STATIC_LIBRARY_SUFFIX}")
        # Rust must target the same minimum macOS as the plugin (otherwise: "built for newer macOS").
        set(_mp_env "")
        if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
            set(_mp_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        add_custom_command(
            OUTPUT "${_mp_lib}"
            COMMAND ${CMAKE_COMMAND} -E env ${_mp_env} "${SMIX_CARGO}" build -p memopro-c --release --target-dir "${_mp_target_dir}"
            WORKING_DIRECTORY "${memopro_SOURCE_DIR}"
            COMMENT "Building memopro-c (Rust)"
            VERBATIM)
        add_custom_target(smix_memopro_build DEPENDS "${_mp_lib}")

        add_library(memopro_c STATIC IMPORTED GLOBAL)
        set_target_properties(memopro_c PROPERTIES
            IMPORTED_LOCATION "${_mp_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${memopro_SOURCE_DIR}/crates/memopro-c/include")
        add_dependencies(memopro_c smix_memopro_build)
        find_package(Threads REQUIRED)
        set(_mp_system_libs Threads::Threads ${CMAKE_DL_LIBS})
        if(UNIX AND NOT APPLE)
            list(APPEND _mp_system_libs m)
        endif()
        if(APPLE)
            # sysinfo / objc2-io-kit (hardware queries) and Metal (unified-memory residency) on Apple.
            list(APPEND _mp_system_libs "-framework CoreFoundation" "-framework Security" "-framework IOKit"
                                        "-framework Metal" "-framework Foundation")
        endif()
        set_property(TARGET memopro_c PROPERTY INTERFACE_LINK_LIBRARIES ${_mp_system_libs})
        set(SMIX_MEMOPRO_ENABLED ON)
        message(STATUS "memopro: enabled (${memopro_SOURCE_DIR})")
    endif()
endif()
