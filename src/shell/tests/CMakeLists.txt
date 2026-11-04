add_executable(shell_tests
    program.test.cpp
    recipe.test.cpp
    descriptor_storage.test.cpp
    valid.test.cpp
    user_value.test.cpp
)

target_link_libraries(shell_tests
    PRIVATE
        clockr_shell
        clockr_kernel
)

add_tests(shell_tests)
enable_warnings(shell_tests)
