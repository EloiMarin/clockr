add_executable(jam_shell jam_shell.cpp)

target_link_libraries(jam_shell
    PRIVATE
        jam
        clockr::replay
        clockr::kernel
        clockr::shell
)

target_link_options(jam_shell PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=JamShell
)

enable_warnings(jam_shell)

configure_file(index.html index.html COPYONLY)
configure_file(style.css style.css COPYONLY)
configure_file(app.js app.js COPYONLY)
configure_file(worker.js worker.js COPYONLY)

if(CLOCKR_BUILD_TESTS)
    add_test(NAME jam.shell.smoke
        COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.js" "$<TARGET_FILE_DIR:jam_shell>")

    add_test(NAME jam.shell.worker
        COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/worker_smoke_test.js" "$<TARGET_FILE_DIR:jam_shell>")
endif()
