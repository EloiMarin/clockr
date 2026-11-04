include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_SHALLOW    TRUE
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.15.0
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)

set(CATCH_INSTALL_DOCS OFF)

set(BUILD_SHARED_LIBS_saved ${BUILD_SHARED_LIBS}) # Save BUILD_SHARED_LIBS
set(BUILD_SHARED_LIBS OFF)

FetchContent_MakeAvailable(Catch2)

set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_saved}) # Restore BUILD_SHARED_LIBS

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)

include(Catch)

macro(add_tests target)
    message("Adding tests to ${target}")
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(${target})
endmacro()
