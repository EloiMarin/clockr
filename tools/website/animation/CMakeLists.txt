# Standalone Emscripten build for the landing-page animation shell.
#
# The site build (tools/website/scripts/build.mjs) configures and builds this
# project and stages clockr_page.js / clockr_page.wasm next to the renderer
# modules under _site/animation/. It is deliberately separate from the root
# build so the native library CI stays untouched; it pulls the library in as
# a subdirectory, exactly like the lab project.

cmake_minimum_required(VERSION 3.28)

project(clockr_landing_animation LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CLOCKR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLOCKR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLOCKR_BUILD_DEBUGGER OFF CACHE BOOL "" FORCE)
set(CLOCKR_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(CLOCKR_BUILD_DEMO_NATIVE OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." clockr)

add_executable(clockr_page src/cursor_clocks.cpp)

target_link_libraries(clockr_page PRIVATE clockr::shell clockr::kernel)

target_compile_options(clockr_page PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>
)

target_link_options(clockr_page PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=ClockrPage
    -fwasm-exceptions
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=8MB
)
