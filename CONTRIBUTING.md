# Contributing to clockr

`clockr` is a C++20 library implementing a real-time-safe, thread-aware FRP
engine for audio applications. Contributions are welcome — this page covers
the environment, the build/test workflow, the coding conventions and the
commit rules. The architecture itself is documented in `doc/`; `AGENTS.md`
records the invariants and the agent-session protocol.

## Environment

- **Nix + direnv (recommended).** Run `direnv allow` in the repository root.
  `.envrc` loads `shell.nix`, which provides clang, clang-tools, lldb, CMake,
  Ninja, pkg-config and Emscripten. `flake.nix` pins the same shell for CI
  (`nix develop .#ci` adds Node, git and the CA bundle).
- **Otherwise:** CMake ≥ 3.28, Ninja, and clang. Clang is the project
  toolchain; native Windows uses MSVC through the `windows_x64_*` presets.
  `cmake/warnings.cmake` has a GCC fallback, but GCC is not a tested path.
- Fresh configurations fetch dependencies (Catch2, tlsf, readerwriterqueue,
  and — with the debugger enabled — uWebSockets and protobuf) over the
  network. Set `CLOCKR_OFFLINE_BUILD=ON` to reuse sources already on disk
  (`FETCHCONTENT_SOURCE_DIR_<name>` points at an on-disk checkout).

On macOS the build uses Apple clang and libc++. `clang-format` may not be
installed there; apply `.clang-format` by hand if the tool is missing.

## Build and test

`build.sh` is the single cross-platform entry point. It selects the presets
for the host (or an explicit `--platform`) and runs configure, build and test:

```bash
./build.sh                               # host platform, release
./build.sh --type debug                  # debug
./build.sh --clean                       # clean build
./build.sh --sanitize address            # address, thread or undefined
./build.sh --platform wasm --type debug  # WebAssembly
```

On Linux it runs `linux_x64_*`, on macOS `macOS_arm64_*`, and on Windows
`windows_x64_*`. Windows builds use Ninja with the ambient MSVC toolchain, so
run `build.sh` from Git Bash with `cl.exe` on `PATH` (for example, a Git Bash
launched from a Developer Command Prompt). The presets only activate on their
host OS; `wasm` must be requested explicitly.

The direct equivalent for a single preset is:

```bash
cmake --workflow linux_x64_debug
cmake --workflow macOS_arm64_debug
```

`ci/ci.sh` is the forge-neutral entry point used by CI; it takes the same
presets and is convenient for reproducing a CI job locally:

```bash
./ci/ci.sh linux_x64_debug
./ci/ci.sh linux_x64_release --no-debugger
./ci/ci.sh linux_x64_debug --sanitize address --no-debugger
```

### WebAssembly

The single-threaded Emscripten build uses its own presets (no pthreads; see
`cmake/emscripten.cmake`). Use the Nix shell, or install the SDK if you are not using Nix:

```bash
nix develop .#ci -c ./build.sh --platform wasm --type debug
# ctest runs under node; both emscripten and node come from the shell

./ci/install-emsdk.sh                          # non-Nix: installs to ~/emsdk
./build.sh --platform wasm --type debug
```

### Debugger

`tools/web-debugger` builds `debugger_demo` (native only, skipped under
Emscripten), gated by `CLOCKR_BUILD_DEBUGGER` (default ON) because it
fetches protobuf and uWebSockets — plus libuv on Windows, where uSockets has
no epoll/kqueue backend. Configure with `-DCLOCKR_BUILD_DEBUGGER=OFF` to
skip it.

### Sanitizers

The sanitizer is a CMake **cache** option, `CLOCKR_SANITIZE`, seeded from
`$ENV{sanitize}` on the first configure. `build.sh --sanitize X` and
`ci/ci.sh --sanitize X` pass `-DCLOCKR_SANITIZE=X` directly. Because it is a
cache option, a reconfiguration can never silently drop instrumentation.
Verify a binary with `ldd <binary> | grep clang_rt` (Linux) or
`otool -L <binary> | grep clang_rt` (macOS).

### Test selection

Tests are Catch2 v3. CTest registers each Catch2 test case individually (via
`add_tests()` in `cmake/catch2.cmake`), not one test per executable:

```bash
ctest --test-dir _build/linux_x64_debug --output-on-failure
ctest --test-dir _build/linux_x64_debug -R "kernel/"   # by test name
```

`-R kernel_tests` matches nothing: the test names are
`<scope> | <Description>`. Test binaries also accept Catch2 filters
(`[engine]`, `-c`) directly.

`version.hpp` is generated into
`_build/<preset>/generated/include/clockr/` from
`cmake/templates/version.hpp.in` and installed as a public header.
`compile_commands.json` is symlinked to the repository root by CMake and is
gitignored.

## Continuous integration

CI logic lives in `ci/ci.sh` and `ci/ci.cmd`, not in workflow YAML. Both
delegate the actual configure/build/test work to `build.sh`;
`ci/ci.cmd` only activates MSVC first. `.github/workflows/ci.yml` is a thin
driver over the preset matrix. See `doc/how-to/ci.md` for the job list and for
migrating the repository to Codeberg (Forgejo Actions or Woodpecker).

## Tests and naming

Add tests with the `add_tests()` macro from `cmake/catch2.cmake` (it links
`Catch2WithMain` and registers with CTest). Test case names follow
`"<scope> | <Description, capitalised>"`. The scope mirrors the module and
optional sub-unit (`kernel | ...`, `shell/program | ...`,
`kernel/topology | ...`, `conc/double_buffer | ...`); suite-relative scopes
such as `integration | ...` are fine. Avoid bare, unscoped or
un-capitalised names, and use the pipe, not `:`.

## Code style and organization

- `.clang-format` is LLVM-derived: 4-space indent, 80 columns, braces on
  their own line for functions and control statements, sorted includes.
- Private members use the `m_` prefix.
- Use `[[nodiscard]]` for functions whose return value should not be ignored.
- Prefer `constexpr` for compile-time-evaluable functions, `noexcept` where
  appropriate, and `static_assert` for template-parameter validation.
- Header-only implementations are `inline`.

## Documentation

- Use `///` for public API documentation. Keep it brief and descriptive;
  rationale, edge cases and "would-be bugs" belong in commit messages or the
  design docs.
- Document template parameters and complex algorithms, and include usage
  examples in class headers where helpful.
- Design decisions live in `doc/`, not in commit messages. When changing
  architecture or temporal/state semantics, update the relevant document
  (`doc/explanation/frp-decisions.md`, `doc/reference/denotational-semantics.md`,
  and the roadmap section of the root `README.md`) in the same change.
- The design docs are pedagogical: preserve the "why", the concrete examples,
  and the reader-first voice.

## Commits

- **Small and self-contained:** one logical change per commit, nothing
  unrelated bundled in.
- Describe the **rationale** in the message — the "why", not just the
  "what".
- Commit **on the fly, never retroactively**: land each green, self-contained
  change as its own commit. Batching an epic and splitting it afterwards is
  error-prone because hunks from different changes interleave in the same
  files.
- Agent-produced commits carry a trailer reading
  `Assisted-by: <model name>`.
- If a change is risky or you are unsure it is correct, stop and ask before
  building further on top of it.

## Consuming the library

`doc/how-to/consuming.md` documents both integration modes (FetchContent and
install + `find_package(clockr)`), the installed layout and the build
options. `doc/examples/host` is the smallest working host.

## Debugging

The `linux_x64_debug` build is `-O0 -g`; `linux_x64_release` is optimized
without debug info, so point the debugger at the debug binary. Even at `-O0`,
lldb can report a few locals (loop counters, const ids) as "optimized out" —
a clang/LLDB DWARF quirk, not a release build.

`scripts/lldb_formatters.py` registers readable summaries for `kernel::value`
(`unit`, `bool<true>`, `int64_t<42>`, `double<2.5>`) and `kernel::delay_state`
(`current=…, next=…`):

- CLI: `lldb -o 'command script import scripts/lldb_formatters.py' _build/linux_x64_debug/src/kernel/tests/kernel_tests`
- nvim-dap (lldb-dap): add
  `initCommands = { "command script import <repo>/scripts/lldb_formatters.py" }`
  to the launch config.
- Or put the same `command script import` line in `~/.lldbinit`.

## License

The project is MIT-licensed (see `LICENSE.md`, which also carries the
third-party notices). Contributions are accepted under the same terms.
