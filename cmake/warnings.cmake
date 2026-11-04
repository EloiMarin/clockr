# warnings.cmake — target-local compiler warnings.
#
# `enable_warnings(<target>)` turns on the compiler's full warning set for a
# given target. The warning set assumes clang (the project toolchain); GCC
# falls back to the portable -Wall/-Wextra/-Wpedantic set, and MSVC to /W4
# plus the individual diagnostics that map onto the clang set.
#
# Every first-party target (module libraries, test executables) should call
# this function. Third-party code (the Catch2 and readerwriterqueue targets,
# the tlsf sources compiled into clockr_mem, protobuf/uWebSockets) is
# explicitly excluded: their sources are not expected to be warning-clean.
#
# To promote warnings to errors it is enough to configure with
# -DCLOCKR_WARNINGS_AS_ERRORS=ON (or set the cache variable); the flag is applied
# inside the function, so each target inherits it automatically.

option(CLOCKR_WARNINGS_AS_ERRORS "Compile warnings as errors" OFF)

function(enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # -Weverything subsumes -Wall/-Wextra/-Wpedantic and adds the clang
        # extras: -Wconversion, -Wshadow, -Wfloat-equal, ... The suppressions
        # below are deliberate:
        #
        # - c++98/c++11 compatibility families: unreasonably noisy for a C++20
        #   codebase (fires on core idioms: auto, lambdas, CTAD); not fixable
        #   without rewriting the language level.
        # - padded: struct layout padding is a property of the ABI, not a bug.
        # - weak-vtables: impure-virtual classes are by design here (recipe
        #   keeps a default out-of-line dtor; the warning asks for key
        #   functions that the class layout does not need).
        # - missing-variable-declarations: fires on file-scope const globals
        #   that need no external linkage in C++.
        # - unsafe-buffer-usage (and the libc-call variant): a work-in-progress
        #   clang analysis that fires inside the readerwriterqueue and Catch2
        #   headers that our translation units compile; it does not currently
        #   fire anywhere in clockr's own code.
        # - covered-switch-default: readerwriterqueue's switches over enum
        #   ranges carry an asserting default; deliberate there.
        # - float-equal: fires inside Catch2's expression decomposer, which
        #   compares operands with ==; tests also legitimately compare exact
        #   round-tripped doubles.
        set(warnings
            -Wall
            -Wextra
            -Wpedantic
            -Weverything
            -Wno-c++98-compat
            -Wno-c++98-c++11-compat
            -Wno-c++98-c++11-c++14-compat
            -Wno-c++98-c++11-c++14-compat-pedantic
            -Wno-c++98-c++11-compat-pedantic
            -Wno-c++98-compat-pedantic
            -Wno-padded
            -Wno-weak-vtables
            -Wno-missing-variable-declarations
            -Wno-unsafe-buffer-usage
            -Wno-unsafe-buffer-usage-in-libc-call
            -Wno-covered-switch-default
            -Wno-float-equal
            -Wno-poison-system-directories
        )

        # shorten-64-to-32 on 32-bit targets only (wasm32): node_id is
        #   64-bit while size_t is 32-bit, so every vector index by id
        #   narrows. A graph can never approach 2^32 nodes inside a 4 GB
        #   address space, so this is accepted per-platform noise (see
        #   cmake/emscripten.cmake), not a per-site fix. It must sit after
        #   -Weverything in this list: directory-level options lose to target
        #   options on the command line, so cmake/emscripten.cmake cannot set
        #   it.
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            list(APPEND warnings -Wno-shorten-64-to-32)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        # MSVC has no -Weverything analogue. /W4 is the broad baseline (roughly
        # -Wall/-Wextra) and each /w1XXXX knob enables one diagnostic that maps
        # onto a clang warning from the set above. /permissive- turns on
        # standards conformance (the -Wpedantic analogue) and /sdl adds the
        # extra security-oriented checks.
        set(warnings
            /W4                  # baseline warnings (roughly -Wall)
            /w14242              # conversion: possible loss of data (-Wconversion)
            /w14263              # member function doesn't override (-Woverloaded-virtual)
            /w14265              # class has virtual functions, non-virtual dtor (-Wnon-virtual-dtor)
            /w14287              # unsigned/negative constant mismatch (-Wsign-conversion)
            /we4289              # nonstandard extension used (-Wpedantic), as error
            /w14296              # expression is always true/false (-Wsign-compare)
            /w14355              # 'this' pointer in initializer list (-Wstrict-aliasing)
            /w14738              # 'operator': result may be uninitialized (-Wcast-align)
            /w14826              # conversion is sign-extended (-Wunreachable-code)
            /w14905              # wide string literal cast to 'LPSTR' (-Wcast-align)
            /w14906              # string literal cast to 'LPWSTR' (-Wcast-align)
            /w14928              # illegal copy-initialization (-Wshadow)
            /permissive-         # standards conformance mode (-Wpedantic)
            /sdl                 # enable additional security checks
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Wold-style-cast
        )
    else()
        # Unknown compiler: keep a conservative portable set rather than error
        # out; the project toolchain is clang, so this path is unlikely.
        set(warnings -Wall -Wextra -Wpedantic)
    endif()

    if(CLOCKR_WARNINGS_AS_ERRORS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            list(APPEND warnings /WX)
        else()
            list(APPEND warnings -Werror)
        endif()
    endif()

    # INTERFACE libraries (header-only code) have no compilation of their own:
    # their warnings must propagate to whoever compiles the headers.
    get_target_property(target_type ${target} TYPE)

    if(target_type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${target} INTERFACE ${warnings})
    else()
        target_compile_options(${target} PRIVATE ${warnings})
    endif()
endfunction()