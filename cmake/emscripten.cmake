# emscripten.cmake — Emscripten (WebAssembly) build support.
#
# Configure with `emcmake cmake --preset wasm32_debug` (emcmake injects the
# toolchain file, so no install-layout-specific path lives in the presets).
# Single-threaded: the library never spawns threads (std::thread appears only
# in stress tests), so the default build needs no pthreads, no
# SharedArrayBuffer, and no COOP/COEP headers. Threaded hammer cases are
# compiled out via CLOCKR_NO_THREADS; everything else — including the DAW
# acceptance tests — runs under node.
#
# Exceptions are load-bearing here (validation, arity checks), so the build
# uses standardized Wasm exceptions rather than Emscripten's legacy JS-based
# mechanism.
#
# No C++20 modules anywhere in the tree, so module dependency scanning is
# off globally (set(CMAKE_CXX_SCAN_FOR_MODULES OFF) in the root CMakeLists):
# with Ninja it would shell out to a scan-deps wrapper neither the Nix
# Emscripten package nor the native Nix cc-wrapper environment can satisfy,
# failing the build for want of a feature nothing uses.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "cmake/emscripten.cmake is only for Emscripten builds (configure with emcmake)")
endif()

option(CLOCKR_NO_THREADS
    "Compile out test cases that spawn threads (single-threaded targets)"
    ON
)

# Standardized Wasm exception handling, compile and link. The compile option
# is C++-only: tlsf builds as C in the same tree and would warn on it.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>)
add_link_options(-fwasm-exceptions)

# Startup memory: the default is tight for the debug test binaries; growth
# stays enabled (single-threaded, so no shared-memory restriction). The stack
# matches a native main thread (8 MB): recipe composition holds by-value
# arenas through nested operator calls, so construction-time stack dwarfs
# the tick path (which stays lean on purpose). Debug (-O0) spills the most;
# release collapses it.
add_link_options(-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=8MB)

# wasm32's size_t is 32-bit while node_id is 64-bit; the -Wshorten-64-to-32
# acceptance lives in cmake/warnings.cmake (it must follow -Weverything on
# the command line, which only the target-level list can do).

if(CLOCKR_NO_THREADS)
    add_compile_definitions(CLOCKR_NO_THREADS)
endif()
