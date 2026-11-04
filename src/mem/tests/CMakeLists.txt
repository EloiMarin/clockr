add_executable(mem_tests
    pool.test.cpp
)

target_link_libraries(mem_tests
    PRIVATE
        clockr_mem
)

add_tests(mem_tests)
enable_warnings(mem_tests)
