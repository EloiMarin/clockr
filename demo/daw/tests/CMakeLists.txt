add_executable(daw_tests
    transport.test.cpp
    demo.test.cpp
)

target_link_libraries(daw_tests
    PRIVATE
        daw_demo
        clockr::shell
        clockr::kernel
        clockr::kernel
)

add_tests(daw_tests)
enable_warnings(daw_tests)
