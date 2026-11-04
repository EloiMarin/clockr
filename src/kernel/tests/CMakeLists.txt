add_executable(kernel_tests
    node.test.cpp
    region_runtime.test.cpp
    topology.test.cpp
    value.test.cpp
    engine.test.cpp
    region_tick.test.cpp
    trace.test.cpp
    user_value.test.cpp
)

target_link_libraries(kernel_tests
    PRIVATE
        clockr_kernel
        clockr_shell
)

add_tests(kernel_tests)
enable_warnings(kernel_tests)
