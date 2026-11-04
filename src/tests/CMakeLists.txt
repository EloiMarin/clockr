add_executable(integration_tests
    integration.test.cpp
)

# Consume the library exactly as external client code would: the public
# clockr::clockr umbrella target, never individual internal _module targets.
target_link_libraries(integration_tests
    PRIVATE
        clockr::clockr
)

add_tests(integration_tests)
enable_warnings(integration_tests)