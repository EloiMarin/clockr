add_library(clockr_mem STATIC
    arena.cpp
    pool.cpp
    ${tlsf_SOURCE_DIR}/tlsf.c
)

target_include_directories(clockr_mem
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${tlsf_SOURCE_DIR}
)

# tlsf.c is third-party C, exclude it from warnings.
set_source_files_properties(${tlsf_SOURCE_DIR}/tlsf.c
    PROPERTIES
        COMPILE_OPTIONS -w
)

target_compile_features(clockr_mem
    PUBLIC
        cxx_std_20
)

enable_warnings(clockr_mem)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
