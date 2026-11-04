include(FetchContent)

# uWebSockets + uSockets — the debug WebSocket/HTTP server.
#
# Neither upstream ships a CMake build (uWebSockets is header-only C++;
# uSockets is a handful of C files built by a Makefile), so both are
# populated manually and wrapped in targets here.
# Only the pieces a plaintext (no TLS, no compression) server needs are
# compiled:
#
#   * uSockets: src/*.c + the eventing backend — epoll/kqueue on Linux/macOS,
#     libuv on Windows (which has neither). SSL is compiled out with
#     LIBUS_NO_SSL, so the crypto sources are not built; the io_uring/gcd/asio
#     backends are not built either.
#   * uWebSockets: headers only, include/ to the uSockets src dir.
#
# Zlib/libdeflate compression is deliberately off: the debugger streams
# uncompressed protobuf frames on localhost.
#
# Pinned to the stable v20 line of uWebSockets and the matching uSockets
# v0.8.x it was released against.

FetchContent_Declare(uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets
    GIT_TAG        v0.8.8
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_Declare(uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets
    GIT_TAG        v20.80.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_GetProperties(usockets)

if(NOT usockets_POPULATED)
    FetchContent_Populate(usockets)

    if(WIN32)
        # uSockets has no epoll/kqueue on Windows and selects LIBUS_USE_LIBUV
        # itself in libusockets.h; fetch libuv and build its backend.
        include(libuv)
        set(usockets_eventing ${usockets_SOURCE_DIR}/src/eventing/libuv.c)
    else()
        set(usockets_eventing ${usockets_SOURCE_DIR}/src/eventing/epoll_kqueue.c)
    endif()

    add_library(usockets STATIC
        ${usockets_SOURCE_DIR}/src/bsd.c
        ${usockets_SOURCE_DIR}/src/context.c
        ${usockets_SOURCE_DIR}/src/loop.c
        ${usockets_SOURCE_DIR}/src/socket.c
        ${usockets_SOURCE_DIR}/src/udp.c
        ${usockets_eventing}
    )

    target_include_directories(usockets
        PUBLIC
            ${usockets_SOURCE_DIR}/src
    )

    # No TLS: the crypto sources are excluded and their include is disabled.
    target_compile_definitions(usockets PRIVATE LIBUS_NO_SSL)

    # Make the Windows backend explicit and link libuv. uv_a carries the
    # system libraries the backend needs (ws2_32, iphlpapi, userenv, ...).
    if(WIN32)
        target_compile_definitions(usockets PUBLIC LIBUS_USE_LIBUV)
        target_link_libraries(usockets PUBLIC uv_a)
    endif()

    # C sources are not warning-clean under our -Weverything; exclude them
    # (the target opts out of enable_warnings entirely).
    set_target_properties(usockets PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endif()

FetchContent_GetProperties(uwebsockets)

if(NOT uwebsockets_POPULATED)
    FetchContent_Populate(uwebsockets)

    add_library(uWebSockets INTERFACE)
    target_include_directories(uWebSockets
        INTERFACE
            ${uwebsockets_SOURCE_DIR}/src
    )
    target_link_libraries(uWebSockets INTERFACE usockets)
    # No TLS and no zlib compression: PerMessageDeflate.h guards its
    # #include <zlib.h> behind UWS_NO_ZLIB; without the define it only
    # builds where zlib.h happens to sit on the default include path.
    target_compile_definitions(uWebSockets INTERFACE
        LIBUS_NO_SSL
        UWS_NO_ZLIB
    )

    # uWebSockets headers are third-party and not -Weverything clean; treat
    # them as system includes for consumers.
    set_target_properties(uWebSockets PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${uwebsockets_SOURCE_DIR}/src"
    )
endif()