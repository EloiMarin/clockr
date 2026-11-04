add_library(clockr_conc INTERFACE)

target_include_directories(clockr_conc
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${readerwriterqueue_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/clockr/third_party/readerwriterqueue>
)

target_compile_features(clockr_conc
    INTERFACE
        cxx_std_20
)

set_target_properties(clockr_conc
    PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${readerwriterqueue_SOURCE_DIR}>"
)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
