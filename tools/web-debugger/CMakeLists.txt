# tools/web-debugger — the debugging web UI: a WebSocket server speaking the
# protobuf schema in proto/, plus static web assets in web/.
#
# Native-only: the server needs threads and sockets, neither of which exist in
# the single-threaded Emscripten build (the root CMakeLists never adds this
# subdirectory for EMSCRIPTEN). Gated by CLOCKR_BUILD_DEBUGGER because it
# pulls protobuf and uWebSockets through FetchContent, which is a real cost to
# a configure that only wants the library. The TypeScript client, MCP server
# and dsh plugin are independent packages under tools/ and are not built here.

# --- protobuf schema + generated C++ ----------------------------------------

set(protobuf_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${protobuf_GENERATED_DIR}")

set(protobuf_PB_HEADER "${protobuf_GENERATED_DIR}/clockr_debug.pb.h")
set(protobuf_PB_SOURCE "${protobuf_GENERATED_DIR}/clockr_debug.pb.cc")

add_custom_command(
    OUTPUT "${protobuf_PB_HEADER}" "${protobuf_PB_SOURCE}"
    COMMAND protobuf::protoc
    ARGS
        --cpp_out "${protobuf_GENERATED_DIR}"
        --proto_path "${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${CMAKE_CURRENT_SOURCE_DIR}/proto/clockr_debug.proto"
    DEPENDS
        protobuf::protoc
        "${CMAKE_CURRENT_SOURCE_DIR}/proto/clockr_debug.proto"
    COMMENT "Generating clockr_debug protobuf C++ sources"
    VERBATIM
)

add_library(clockr_debugger_proto STATIC
    "${protobuf_PB_SOURCE}"
)

target_include_directories(clockr_debugger_proto
    PUBLIC
        "${protobuf_GENERATED_DIR}"
)

# Generated protobuf headers use reserved identifiers by design
# (_Internal, _InternalSerialize); treat the generated directory as a system
# include so the project's -Weverything does not diagnose it.
set_target_properties(clockr_debugger_proto PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${protobuf_GENERATED_DIR}"
)

target_link_libraries(clockr_debugger_proto
    PUBLIC
        protobuf::libprotobuf
)

# Generated protobuf sources are not warning-clean under -Weverything; suppress
# warnings for them (the codec that *uses* the generated types is warned
# normally and calls enable_warnings itself).
target_compile_options(clockr_debugger_proto PRIVATE -w)

# --- server library + demo executable ---------------------------------------

add_library(clockr_debugger STATIC
    server/recorder.cpp
    server/server.cpp
    server/graph_message.cpp
    server/event_message.cpp
)

target_include_directories(clockr_debugger
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(clockr_debugger
    PUBLIC
        clockr::kernel
        clockr::shell
        clockr_debugger_proto
    PRIVATE
        uWebSockets
)

enable_warnings(clockr_debugger)

# uWebSockets/uSockets are third-party; their targets are not warned.

# debugger_demo is a small host that exercises the server: an embedding host
# only needs the clockr_debugger library above, but the demo makes the web UI
# and the MCP workflow runnable out of the box.
add_executable(debugger_demo
    main.cpp
)

target_link_libraries(debugger_demo
    PRIVATE
        clockr_debugger
)

enable_warnings(debugger_demo)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
