include(FetchContent)

# miniaudio — the native demo's audio device backend.
#
# One C translation unit with runtime-linked backends (ALSA/PulseAudio/JACK on
# Linux, CoreAudio on macOS, WASAPI/DSOUND/WinMM on Windows) and a null backend
# that drives the same callback on a real thread when no device exists. Only
# device I/O is needed by the demo: examples, tests, tools, extra nodes and the
# high-level engine/decoder APIs are switched off to keep the dependency small.
#
# Upstream ships a CMake build, but no install/export set is wanted here — the
# native demo is a tool, not part of the library's installed package.
set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_INSTALL OFF CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_EXTRA_NODES ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_ENGINE ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_DECODING ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_ENCODING ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_RESOURCE_MANAGER ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_NODE_GRAPH ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_GENERATION ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL "" FORCE)

FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio
    GIT_TAG        0.11.25
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(miniaudio)

# miniaudio's headers are not -Weverything clean; treat them as system headers
# for the demo that includes them. Its own C translation unit keeps upstream's
# warning flags (this project's enable_warnings is never applied to it).
set_target_properties(miniaudio PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${miniaudio_SOURCE_DIR}"
)

# miniaudio's CMake does not add the platform frameworks/system libraries its
# CoreAudio/WASAPI backends need, so attach them to the target for consumers.
if(APPLE)
    target_link_libraries(miniaudio
        INTERFACE
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework CoreFoundation"
    )
elseif(WIN32)
    target_link_libraries(miniaudio
        INTERFACE
            ole32
            winmm
    )
endif()
