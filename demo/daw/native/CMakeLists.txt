# Native DAWmulator host: the same recipe and page as the wasm demo, but with
# real OS threads (a ui thread plus miniaudio's device callback) and a browser
# connected over WebSocket. Native-only: miniaudio and uWebSockets both need
# threads/sockets that the single-threaded Emscripten build does not have.
#
# When the debugger is built (CLOCKR_BUILD_DEBUGGER, default ON) the host
# also embeds debug::server, so the MCP workflow can attach to the same live
# program at ws://127.0.0.1:9002/ws. This target is added after tools/web-debugger
# so it can link that server library; without it the demo still runs, serving
# only the page.

add_executable(dawmulator_native
    main.cpp
    host.cpp
    audio.cpp
    synth.cpp
    server.cpp
)

target_link_libraries(dawmulator_native
    PRIVATE
        daw_demo
        clockr::clockr
        uWebSockets
        miniaudio
        miniaudio_common_options
)

target_compile_definitions(dawmulator_native
    PRIVATE
        DAW_NATIVE_WEB_ROOT="${CMAKE_CURRENT_BINARY_DIR}/web"
)

if(TARGET clockr_debugger AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/web-debugger/server/commands.hpp")
    target_sources(dawmulator_native PRIVATE debug.cpp)
    target_link_libraries(dawmulator_native PRIVATE clockr_debugger)
    target_compile_definitions(dawmulator_native PRIVATE DAW_NATIVE_WITH_DEBUGGER=1)

    # The debugger's own web UI is source, not a build output: point the
    # native host's default --debug-web-root at it rather than at the cwd.
    get_filename_component(
        daw_debug_web_root
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/web-debugger/web"
        ABSOLUTE
    )
    target_compile_definitions(dawmulator_native
        PRIVATE
            DAW_DEBUG_WEB_ROOT="${daw_debug_web_root}"
    )
endif()

enable_warnings(dawmulator_native)

# Stage the shared page next to the executable. The native server serves this
# directory by default; --web-root overrides it.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/web")

set(daw_web_source "${CMAKE_CURRENT_SOURCE_DIR}/../shell")

foreach(file index.html style.css app.js engine_worker.js)
    configure_file(
        "${daw_web_source}/${file}"
        "${CMAKE_CURRENT_BINARY_DIR}/web/${file}"
        COPYONLY
    )
endforeach()

if(CLOCKR_BUILD_TESTS)
    add_test(NAME daw.native.smoke COMMAND dawmulator_native --selftest)
    set_tests_properties(daw.native.smoke PROPERTIES TIMEOUT 30)
endif()
