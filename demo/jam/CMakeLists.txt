add_library(jam INTERFACE)

target_include_directories(jam
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(jam
    INTERFACE
        clockr::shell
        clockr::kernel
        clockr::replay
        daw_demo
)

enable_warnings(jam)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(EMSCRIPTEN)
    add_subdirectory(shell)
endif()
