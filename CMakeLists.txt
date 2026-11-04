cmake_minimum_required(VERSION 3.28)

project(clockr
    LANGUAGES CXX C
    VERSION 0.1.0
)

option(CLOCKR_OFFLINE_BUILD
    "Do not access the network to update dependencies during the build"
    OFF
)

option(CLOCKR_BUILD_TESTS
    "Build clockr's test suites (fetches Catch2)"
    ${PROJECT_IS_TOP_LEVEL}
)

option(CLOCKR_BUILD_EXAMPLES
    "Build the usage examples"
    ${PROJECT_IS_TOP_LEVEL}
)

option(CLOCKR_BUILD_DEBUGGER
    "Build the web debugging server and demo (native only; fetches protobuf + uWebSockets)"
    ${PROJECT_IS_TOP_LEVEL}
)

option(CLOCKR_BUILD_DEMO
    "Build the DAWmulator demo"
    ${PROJECT_IS_TOP_LEVEL}
)

option(CLOCKR_BUILD_DEMO_NATIVE
    "Build the native DAWmulator host (fetches miniaudio + uWebSockets; native only)"
    ${PROJECT_IS_TOP_LEVEL}
)

set(CLOCKR_DAW_NATIVE_ENABLED OFF)

if(CLOCKR_BUILD_DEMO AND CLOCKR_BUILD_DEMO_NATIVE AND NOT EMSCRIPTEN)
    set(CLOCKR_DAW_NATIVE_ENABLED ON)
endif()

# BUILD_TESTING=OFF is the standard global "no tests" signal: honour it as a veto over CLOCKR_BUILD_TESTS.
if(DEFINED BUILD_TESTING AND NOT BUILD_TESTING)
    set(CLOCKR_BUILD_TESTS OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0")
endif()

set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ${CLOCKR_OFFLINE_BUILD})

include(tlsf)
include(readerwriterqueue)
include(warnings)

if(EMSCRIPTEN)
    include(emscripten)
endif()

if(PROJECT_IS_TOP_LEVEL)
    include(link_compile_commands)
endif()

if(CLOCKR_BUILD_TESTS)
    include(CTest)
    include(catch2)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/clockr/version.hpp"
)

set(
    CLOCKR_SANITIZE "$ENV{sanitize}"
    CACHE STRING "Sanitizer to build with (address|thread|undefined|none)"
)

if(NOT "${CLOCKR_SANITIZE}" STREQUAL "none" AND NOT "${CLOCKR_SANITIZE}" STREQUAL "")
    message(STATUS "Enabling sanitizer: ${CLOCKR_SANITIZE}")
    if(MSVC)
        # MSVC spells the sanitizer flag differently and only ships AddressSanitizer.
        if(NOT "${CLOCKR_SANITIZE}" STREQUAL "address")
            message(FATAL_ERROR "MSVC only supports --sanitize address (got '${CLOCKR_SANITIZE}')")
        endif()
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=${CLOCKR_SANITIZE} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${CLOCKR_SANITIZE})
    endif()
endif()

set(CLOCKR_DEBUGGER_ENABLED OFF)

if(CLOCKR_BUILD_DEBUGGER AND NOT EMSCRIPTEN)
    set(CLOCKR_DEBUGGER_ENABLED ON)
endif()

if(CLOCKR_DEBUGGER_ENABLED)
    include(protobuf)
endif()

if(CLOCKR_DEBUGGER_ENABLED OR CLOCKR_DAW_NATIVE_ENABLED)
    include(uwebsockets)
endif()

if(CLOCKR_DAW_NATIVE_ENABLED)
    include(miniaudio)
endif()

add_subdirectory(src)

if(CLOCKR_BUILD_DEMO)
    add_subdirectory(demo/daw)
endif()

if(CLOCKR_BUILD_EXAMPLES)
    add_subdirectory(doc/examples)
endif()

if(CLOCKR_DEBUGGER_ENABLED)
    add_subdirectory(tools/web-debugger)
endif()

if(CLOCKR_DAW_NATIVE_ENABLED)
    add_subdirectory(demo/daw/native)
endif()

if(PROJECT_IS_TOP_LEVEL)
    include(clockr_install)
endif()
