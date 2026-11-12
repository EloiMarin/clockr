add_library(clockr_replay STATIC
    scenario.cpp
    value_codec.cpp
    session.cpp
    replayer.cpp
    compare.cpp
)

target_include_directories(clockr_replay
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(clockr_replay
    PUBLIC
        cxx_std_20
)

target_link_libraries(clockr_replay
    PUBLIC
        clockr_kernel
)

enable_warnings(clockr_replay)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
