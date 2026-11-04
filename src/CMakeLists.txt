add_subdirectory(mem)
add_subdirectory(conc)
add_subdirectory(kernel)
add_subdirectory(shell)

add_library(clockr INTERFACE)

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

if(CLOCKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
