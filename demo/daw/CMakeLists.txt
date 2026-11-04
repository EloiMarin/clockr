add_library(daw_demo INTERFACE)

target_include_directories(daw_demo
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(daw_demo
    INTERFACE
        clockr::shell
        clockr::kernel
        clockr::kernel
)

enable_warnings(daw_demo)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(EMSCRIPTEN)
    add_subdirectory(shell)
endif()
