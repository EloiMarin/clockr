# Consuming clockr from another CMake project

`clockr` is a plain C++20 static library with a CMake package. A host
project — the application that creates a `kernel::program` and steps it —
integrates it in one of two ways. Both spell the dependency the same way;
they differ only in where the library comes from.

```cmake
target_link_libraries(app PRIVATE clockr::clockr)
```

That single line is the whole integration. Everything else on this page
explains what it pulls in and how the two modes provide it.

## What `clockr::clockr` gives you

A CMake *target* is not just a `.a` file: it carries *usage requirements*
that flow into anything linking it. `clockr::clockr` is an `INTERFACE`
umbrella over four module targets:

| Target | What it is | Headers |
| --- | --- | --- |
| `clockr::clockr` | umbrella (no artifact of its own) | `clockr/version.hpp` |
| `clockr::shell` | typed composition layer (`recipe`, operators) | `clockr/shell/...` |
| `clockr::kernel` | erased execution plane (`program`, engines) | `clockr/kernel/...` |
| `clockr::conc` | header-only clock-crossing primitives | `clockr/conc/...` |
| `clockr::mem` | pool allocation for the real-time path | `clockr/mem/...` |

Linking the umbrella links the four modules. Linking a module directly
(`clockr::kernel`, …) also works — each carries exactly its own include
dir, link closure, and requirements.

The requirements that travel with these targets:

- **Include root.** Headers are included as `"clockr/kernel/graph.hpp"`,
  `"clockr/shell/recipe.hpp"`, … — one `clockr/`-namespaced root, the
  same spelling in both consumption modes.
- **C++20.** Each target declares `cxx_std_20`, so the host gets at least
  C++20 without setting `CMAKE_CXX_STANDARD` itself.
- **Static, position-independent.** The archives are built with PIC, so they
  link into host shared libraries too.
- **No transitive third-party targets.** `tlsf` is compiled into
  `clockr_mem`; the moodycamel queue headers ship inside the package. A
  host never fetches, finds, or links anything else.

`doc/examples/host/main.cpp` is the smallest working host: a recipe, a program,
one engine step. Copy it as a starting point.

## Mode A — build from source with FetchContent (recommended)

```cmake
include(FetchContent)
FetchContent_Declare(clockr
    GIT_REPOSITORY https://github.com/<you>/clockr
    GIT_TAG        <tag>
)
FetchContent_MakeAvailable(clockr)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE clockr::clockr)
```

This builds clockr as a subdirectory of your project, pinned to the tag.
Clockr detects the embedding (`PROJECT_IS_TOP_LEVEL` is false) and stays
out of the way: tests, examples, and the debugger default **off**, and
its `option()`s are `CLOCKR_`-prefixed so they cannot collide with yours.
Catch2 is only fetched when tests are enabled.

Useful overrides for the embedding host:

```cmake
set(CLOCKR_BUILD_TESTS ON)   # build clockr's own test suites too
set(CLOCKR_OFFLINE_BUILD ON) # never touch the network for dependencies
```

`BUILD_TESTING=OFF` is honoured as a global "no tests" veto.

## Mode B — install once, `find_package` everywhere

```bash
cmake -S <clockr> -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCLOCKR_BUILD_TESTS=OFF \
    -DCLOCKR_BUILD_EXAMPLES=OFF \
    -DCLOCKR_BUILD_DEBUGGER=OFF \
    -DCMAKE_INSTALL_PREFIX=/opt/clockr
cmake --build build
cmake --install build
```

(The three `=OFF` flags skip everything that is not the library: tests need
Catch2, the debugger needs protobuf + uWebSockets (and libuv on Windows), and
none of it is installed.)

```cmake
find_package(clockr 0.1 CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE clockr::clockr)
```

```bash
cmake -S app -B app/build -DCMAKE_PREFIX_PATH=/opt/clockr
```

`find_package` searches `CMAKE_PREFIX_PATH` (plus standard locations) for
`lib/cmake/clockr/clockrConfig.cmake`. The installed tree is
self-contained:

```
<prefix>/include/clockr/{mem,conc,kernel,shell}/**.hpp
<prefix>/include/clockr/version.hpp
<prefix>/include/clockr/third_party/readerwriterqueue/
<prefix>/lib/libclockr_{mem,kernel,shell}.a
<prefix>/lib/cmake/clockr/
    clockrConfig.cmake  clockrConfigVersion.cmake
    clockrTargets.cmake clockrTargets-<config>.cmake
<prefix>/share/licenses/clockr/LICENSE.md
```

Notes:

- The version file currently requires the **same minor** version (`0.1`
  satisfies `find_package(clockr 0.1)`; `0.2` will not). This tightens to
  the usual same-major rule after 1.0.
- Developing against an uninstalled build works too:
  `-Dclockr_DIR=<clockr-build-dir>` — the build tree exports an
  equivalent package (same target names, build-tree paths).
- `cmake --install` never drags in Catch2 or the debugger's dependencies:
  Catch2 self-guards as a subproject and the rest are `EXCLUDE_FROM_ALL` or
  simply not enabled.

## Which mode?

FetchContent pins the exact source your app builds against and needs no
install step — best for development and CI. An installed package decouples
builds, needs no network at host-configure time, and carries no compiler
flags from the dependency — best for distributing or vendoring. The consumed
target names are identical, so switching later is a build-system-only change.

## If the library is ever renamed

The name `clockr` appears in exactly these places: the `project()` name
(CMakeLists.txt), the `include/clockr/` directory and the `"clockr/..."`
include prefix, the `clockr::` target namespace and `EXPORT_NAME`s
(`cmake/clockr_install.cmake`), the package config filenames
(`cmake/clockrConfig.cmake.in`, `lib/cmake/clockr/`), the
`CLOCKR_*` option prefix, and this file. A rename is a mechanical sweep
of that list.

## Troubleshooting

- **CMake version.** Clockr requires CMake 3.28+ (for `PROJECT_IS_TOP_LEVEL`
  semantics and `EXCLUDE_FROM_ALL` FetchContent dependencies).
- **First configure needs the network** (FetchContent: tlsf,
  readerwriterqueue, plus Catch2/protobuf when enabled). Use
  `CLOCKR_OFFLINE_BUILD=ON` or `FETCHCONTENT_SOURCE_DIR_<name>=<path>`
  once the sources are on disk.
- **Warnings.** Clockr's own `-Weverything -Werror` never leaks into host
  translation units: warning flags live on compiled targets (`PRIVATE`), not
  on the exported interface. (On CMake 4+, embedding prints a one-time
  deprecation notice from moodycamel's own `cmake_minimum_required(3.9)` —
  upstream's file, harmless.)
- **Sanitizers.** The `CLOCKR_SANITIZE` cache option (seeded from
  `$ENV{sanitize}`) applies to clockr's own targets only. A host with its
  own sanitizer setup is unaffected.
- **Link collisions.** `tlsf_*` symbols are compiled into `clockr_mem`. A
  host that also links its own TLSF would get duplicate-symbol errors at
  link time — unlikely, but the failure mode if it ever happens.
- **WebAssembly.** The library builds for single-threaded Emscripten
  (`wasm32_*` presets); the debugger's native WebSocket server stays off
  there. The debugger UI can still attach to a wasm host through its
  in-browser BroadcastChannel transport (the website's DAWmulator demo does).
  Packaging a wasm install is possible but unusual — hosts typically
  FetchContent the source into their Emscripten build instead.
