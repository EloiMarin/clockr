add_executable(replay_tests
    identity.test.cpp
    codec.test.cpp
    session.test.cpp
    replayer.test.cpp
    compare.test.cpp
    adapter.test.cpp
)

target_link_libraries(replay_tests
    PRIVATE
        clockr_replay
        clockr_shell
)

add_tests(replay_tests)
enable_warnings(replay_tests)
