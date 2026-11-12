add_subdirectory(mem)
add_subdirectory(conc)
add_subdirectory(kernel)
add_subdirectory(shell)
add_subdirectory(replay)

add_library(clockr INTERFACE)

# `clockr::replay` is deliberately opt-in: link it explicitly. Replay builds on
# the core invariants; it is not part of the core proposition, so consumers of
# the engine do not inherit it.
target_link_libraries(clockr
    INTERFACE
        clockr_shell
        clockr_kernel
        clockr_conc
        clockr_mem
)

target_include_directories(clockr
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>
)

add_library(clockr::clockr ALIAS clockr)

add_library(clockr::mem ALIAS clockr_mem)
add_library(clockr::conc ALIAS clockr_conc)
add_library(clockr::kernel ALIAS clockr_kernel)
add_library(clockr::shell ALIAS clockr_shell)
add_library(clockr::replay ALIAS clockr_replay)

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
