add_executable(jam_conductor
    main.cpp
    conductor.cpp
)

target_link_libraries(jam_conductor
    PRIVATE
        jam
        clockr::replay
        clockr::kernel
        clockr::shell
        clockr::conc
)

enable_warnings(jam_conductor)

if(CLOCKR_BUILD_TESTS)
    add_test(
        NAME jam.native.selftest
        COMMAND $<TARGET_FILE:jam_conductor> --selftest
    )
    set_tests_properties(jam.native.selftest PROPERTIES TIMEOUT 30)
endif()
