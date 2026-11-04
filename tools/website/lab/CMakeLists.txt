# Throwaway animation lab.
#
# Standalone Emscripten project so it does not touch the root build. It pulls
# the library in as a subdirectory, then compiles every animations/*.cpp into
# one modularized embind module (LabShell).

cmake_minimum_required(VERSION 3.28)

project(clockr_animation_lab LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CLOCKR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLOCKR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLOCKR_BUILD_DEBUGGER OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." clockr)

file(GLOB LAB_ANIMATIONS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/animations/*.cpp"
)

if(NOT LAB_ANIMATIONS)
    message(FATAL_ERROR "No files in animations/; add at least one .cpp")
endif()

add_executable(lab_shell ${LAB_ANIMATIONS})

target_link_libraries(lab_shell PRIVATE clockr::shell clockr::kernel)

target_compile_options(lab_shell PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>
)

target_link_options(lab_shell PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=LabShell
    -fwasm-exceptions
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=8MB
)
