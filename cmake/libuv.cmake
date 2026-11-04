include(FetchContent)

# libuv — the event loop uSockets falls back to on Windows, which has neither
# epoll (Linux) nor kqueue (macOS). Only cmake/uwebsockets.cmake pulls this in,
# and only under WIN32, so the Unix debugger keeps its dependency-free
# epoll/kqueue backend.
#
# Upstream ships a CMake build exposing the static `uv_a` target. Tests,
# benchmarks, and the shared `uv` library are disabled: the debugger links a
# static `uv_a`, and EXCLUDE_FROM_ALL keeps libuv's own install/export rules
# out of clockr's install tree.
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libuv
    GIT_REPOSITORY https://github.com/libuv/libuv
    GIT_TAG        v1.48.0
    GIT_PROGRESS   TRUE
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(libuv)
