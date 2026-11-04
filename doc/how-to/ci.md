# Continuous integration

CI is split into two layers on purpose:

- **`build.sh` — the unified build entry point.** It maps a platform to its
  CMake presets and performs configure, build and test. Everything that takes
  time to get right — sanitizer wiring, the debugger switch, the Emscripten
  bootstrap — lives there.
- **`ci/` — the forge-neutral layer.** `ci/ci.sh` (Unix) and `ci/ci.cmd`
  (Windows, which only activates MSVC first) translate a preset name and
  delegate to `build.sh`.
- **Workflow YAML — the forge-specific layer.** It provides the environment and
  calls the scripts. Linux, WebAssembly and website jobs use the pinned Nix
  devShell (`nix develop .#ci`); macOS and Windows stay native so the
  project keeps testing Apple clang and MSVC. Migrating the repository from
  GitHub to Codeberg means rewriting the YAML, not the build logic.

The same scripts run locally, so a red CI job can be reproduced exactly:

```bash
./ci/ci.sh linux_x64_debug
./ci/ci.sh linux_x64_release --no-debugger
./ci/ci.sh linux_x64_debug --sanitize address --no-debugger

# Exactly what CI runs (pinned Nix devShell: clang, cmake, ninja, emscripten,
# node, git, CA bundle):
nix develop .#ci -c ./ci/ci.sh linux_x64_debug --no-debugger
nix develop .#ci -c ./ci/ci.sh wasm32_debug --no-debugger

# Without Nix, install the Emscripten SDK first:
./ci/install-emsdk.sh
./ci/ci.sh wasm32_debug --no-debugger
```

`build.sh` (and therefore `ci/ci.sh`) passes `CLOCKR_SANITIZE` and
`CLOCKR_BUILD_DEBUGGER` as CMake cache variables (`-D`), not environment
variables. That avoids the reconfigure trap described in `../AGENTS.md`: a
build directory configured without a sanitizer can never silently be reused
as a sanitized one.

## What the GitHub workflow runs

`.github/workflows/ci.yml` mirrors the project's support matrix:

| Job | Presets | Extra |
| --- | --- | --- |
| Linux | `linux_x64_debug`, `linux_x64_release` | debugger off |
| Linux debugger | `linux_x64_debug` | debugger + node smoke tests on |
| Sanitizers | `linux_x64_debug` | `address`, `thread`, `undefined` |
| macOS | `macOS_arm64_debug`, `macOS_arm64_release` | debugger off |
| WebAssembly | `wasm32_debug`, `wasm32_release` | builds the wasm library and runs node tests |
| Windows | `windows_x64_debug`, `windows_x64_release` | MSVC via `vcvars64.bat` |

The Linux, WebAssembly and website jobs run inside the flake's `ci` devShell,
so clang, CMake, Ninja, Emscripten and Node are pinned by `flake.lock`
(`nixpkgs` input). macOS keeps the runner's Apple clang and Windows uses MSVC
through `vcvars64.bat`; those toolchains are part of the support matrix, not
things Nix should replace. The project builds with `-Weverything -Werror`, so
a new clang can still surface a new warning on a Nix update — update
`flake.lock` deliberately, not implicitly.

## Migrating to Codeberg

Codeberg offers two common CI systems. Either way, keep the `ci/` scripts and
translate only the workflow:

### Forgejo Actions (GitHub-Actions-compatible)

Create `.forgejo/workflows/ci.yml`. The jobs are almost identical to the
GitHub file; the differences are the checkout action and the runner labels:

```yaml
on: [push, pull_request]

jobs:
  linux:
    runs-on: docker            # label configured on the Codeberg runner
    strategy:
      fail-fast: false
      matrix:
        preset: [linux_x64_debug, linux_x64_release]
    steps:
      - uses: https://code.forgejo.org/actions/checkout@v4
      - run: ./ci/ci.sh ${{ matrix.preset }} --no-debugger
```

Notes:

- Codeberg's hosted runners are **Linux only**. The macOS, Windows and
  sanitizer jobs need self-hosted runners (or can stay on GitHub until the
  release is fully moved).
- Use the checkout action from `code.forgejo.org` (or the one mirrored on
  your instance) rather than `github.com`, so the public release does not
  depend on the forge it is migrating away from.
- `install-emsdk.sh` clones from GitHub. If that dependency matters for the
  release, mirror `emscripten-core/emsdk` and point the script at the mirror.

### Woodpecker

Woodpecker clones the repository into the workspace itself, so a minimal
pipeline is:

```yaml
pipeline:
  linux:
    image: ubuntu:24.04
    commands:
      - apt-get update
      - apt-get install -y clang cmake ninja-build git
      - ./ci/ci.sh linux_x64_debug --no-debugger
```

Add one pipeline per matrix entry, and a `wasm` pipeline that installs
Python and calls `ci/install-emsdk.sh` before `ci/ci.sh wasm32_debug`.

## Deliberate omissions

- **No build caching yet.** The Nix jobs currently download their toolchain
  from `cache.nixos.org` on every run; adding `nix-community/cache-nix-action`
  (or a Cachix cache) is the obvious next step once the job times matter. The
  CMake `FetchContent` dependencies are the other cache target.
- **No release or page deployment.** Tagging and publishing are release-time
  decisions, and the demo page is deployed independently of the test matrix.
