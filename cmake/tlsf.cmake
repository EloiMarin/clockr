include(FetchContent)

# Upstream mattconte/tlsf has no build system of its own (single tlsf.c/tlsf.h),
# so it is only populated here — never added as a subdirectory (there is no
# CMakeLists upstream, so FetchContent_MakeAvailable just populates). The
# sources are compiled straight into clockr_mem (see
# src/mem/CMakeLists.txt), which keeps the installed package free of
# a separate tlsf target.
FetchContent_Declare(tlsf
    GIT_REPOSITORY https://github.com/mattconte/tlsf
    GIT_TAG        deff9ab509341f264addbd3c8ada533678591905
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_MakeAvailable(tlsf)
