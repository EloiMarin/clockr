add_library(clockr_shell STATIC
    recipe.cpp
)

target_include_directories(clockr_shell
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(clockr_shell
    PUBLIC
        cxx_std_20
)

target_link_libraries(clockr_shell
    PUBLIC
        clockr_kernel
)

enable_warnings(clockr_shell)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
