include(FetchContent)

# protobuf — the schema for the debug wire protocol. Built from source so the
# matching `protoc` is available as a host tool for code generation (no system
# package needed). Pinned to the 6.31 line (libprotobuf 6.x, libprotoc 31.x),
# which vendors a modern abseil whose Apple single-arch copts work with a
# single-arch Ninja build.
#
# Only the pieces the debugger needs are built: no tests, no examples, no
# install rule. Abseil is fetched by protobuf's own CMake (it has no vendored
# copy in-tree) because the host has no system abseil.
#
# `protobuf::libprotobuf` is the target to link against; `protoc` is used via
# $<TARGET_FILE:protoc> in tools/web-debugger.
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB ON CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)

# Fetch abseil via protobuf's own FetchContent fallback rather than requiring a
# system package.
set(protobuf_FORCE_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)

FetchContent_Declare(protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf
    GIT_TAG        v6.31.1
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_MakeAvailable(protobuf)

# protobuf and abseil headers are third-party and not warning-clean under the
# project's -Weverything: promote their include directories to SYSTEM so our
# targets (which include the generated headers) stop diagnosing them.
foreach(_protobuf_target libprotobuf libprotobuf-lite libprotoc)
    if(TARGET ${_protobuf_target})
        get_target_property(_protobuf_includes ${_protobuf_target} INTERFACE_INCLUDE_DIRECTORIES)

        if(_protobuf_includes)
            set_target_properties(${_protobuf_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_protobuf_includes}")
        endif()
    endif()
endforeach()