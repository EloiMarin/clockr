add_executable(jam_tests
    score.test.cpp
    network.test.cpp
    session.test.cpp
    metrics.test.cpp
    replay.test.cpp
)

target_link_libraries(jam_tests
    PRIVATE
        jam
        clockr::shell
        clockr::kernel
        clockr::replay
)

add_tests(jam_tests)
enable_warnings(jam_tests)
