# doc/examples/ — minimal host programs that consume clockr the way an
# external project would: public headers plus the clockr::clockr umbrella
# target, nothing internal. Each example doubles as a smoke test of the
# consumption path (see doc/how-to/consuming.md). They live under doc/ so the
# repository root stays a small, obvious set of entry points.

add_executable(clockr_host_example
    host/main.cpp
)

target_link_libraries(clockr_host_example
    PRIVATE
        clockr::clockr
)

enable_warnings(clockr_host_example)

add_executable(clockr_first_network
    first_network/main.cpp
)

target_link_libraries(clockr_first_network
    PRIVATE
        clockr::clockr
)

enable_warnings(clockr_first_network)

if(CLOCKR_BUILD_TESTS)
    add_test(NAME clockr_host_example COMMAND clockr_host_example)
    add_test(NAME clockr_first_network COMMAND clockr_first_network)
endif()

