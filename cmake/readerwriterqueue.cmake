include(FetchContent)

FetchContent_Declare(readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue
    GIT_TAG        v1.0.7
    GIT_PROGRESS   TRUE
    GIT_SHALLOW    TRUE
    # Upstream ships install rules of its own; excluded here because clockr
    # bundles the (header-only) dependency into its own install tree instead
    # (see cmake/clockr_install.cmake).
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(readerwriterqueue)

# Only ${readerwriterqueue_SOURCE_DIR} is consumed (as a build-tree include
# dir on clockr_conc, and as the files bundled into the installed package).
# The upstream target itself is intentionally left unlinked so it never enters
# an export set; the system-header marking for its atomics layer (branches,
# e.g. the ThreadSanitizer one, that would trip this project's -Weverything)
# lives on clockr_conc instead.
