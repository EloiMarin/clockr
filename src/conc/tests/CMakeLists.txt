add_executable(conc_tests
    mpsc_stack.test.cpp
    ref_transfer.test.cpp
    slot.test.cpp
    link.test.cpp
)

target_link_libraries(conc_tests
    PRIVATE
        clockr_conc
)

add_tests(conc_tests)
enable_warnings(conc_tests)
