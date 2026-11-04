add_library(clockr_kernel STATIC
    program.cpp
    engine.cpp
    debug_engine.cpp
    release_engine.cpp
    trace.cpp
)

target_include_directories(clockr_kernel
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(clockr_kernel
    PUBLIC
        cxx_std_20
)

target_link_libraries(clockr_kernel
    PUBLIC
        clockr_conc
        clockr_mem
)

enable_warnings(clockr_kernel)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
