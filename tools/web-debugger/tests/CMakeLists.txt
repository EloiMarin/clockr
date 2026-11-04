add_executable(debugger_tests
    command.test.cpp
    message.test.cpp
    recorder.test.cpp
    server.test.cpp
)

target_link_libraries(debugger_tests
    PRIVATE
        clockr_debugger
)

add_tests(debugger_tests)
enable_warnings(debugger_tests)

# The web client's protobuf decoder and graph layout are plain ESM run by the
# browser; exercise both under node so the JS half stays tested in CI (the
# layout test loads the vendored ELK bundle). Node is only required for these
# tests (the same dependency the wasm shell has).
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME debugger.schema.smoke
        COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../web/schema_smoke_test.js")
    add_test(NAME debugger.layout.smoke
        COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../web/layout_smoke_test.js")
else()
    message(STATUS "node not found: skipping the debugger JS smoke tests")
endif()