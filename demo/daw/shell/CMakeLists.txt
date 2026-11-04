# Web shell: embind bindings plus the demo page. Emscripten-only; the C++
# tests stay the acceptance bar, this is the same contract over JS.
add_executable(daw_shell daw_shell.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/web-debugger/server/recorder.cpp"
)

target_include_directories(daw_shell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/web-debugger"
)

target_link_libraries(daw_shell PRIVATE daw_demo)

# Modularized factory (DawShell().then(...)) so the page and node load the
# same module the same way.
target_link_options(daw_shell PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=DawShell
)

enable_warnings(daw_shell)

# The page loads ./daw_shell.js relatively: stage a copy next to the built
# module so the build dir serves the whole site as-is.
configure_file(index.html index.html COPYONLY)
configure_file(style.css style.css COPYONLY)
configure_file(app.js app.js COPYONLY)
configure_file(engine_worker.js engine_worker.js COPYONLY)

# Node smoke tests: the embind surface (daw.shell.smoke), the worker
# protocol the page speaks (daw.shell.worker), and the importScripts
# bootstrap a real browser worker takes (daw.shell.bootstrap) — keeping the
# website path green in CI alongside the C++ suite.
add_test(NAME daw.shell.smoke
    COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.js" "$<TARGET_FILE_DIR:daw_shell>")

add_test(NAME daw.shell.worker
    COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/worker_smoke_test.js" "$<TARGET_FILE_DIR:daw_shell>")

add_test(NAME daw.shell.bootstrap
    COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap_smoke_test.js" "$<TARGET_FILE_DIR:daw_shell>")

add_test(NAME daw.shell.local_debug
    COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/local_debug_test.js" "$<TARGET_FILE_DIR:daw_shell>")
