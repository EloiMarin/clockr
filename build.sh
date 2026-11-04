#!/usr/bin/env bash

set -euo pipefail

# Single cross-platform entry point for building and testing clockr. It maps a
# platform name to the CMake presets in CMakePresets.json, then configures,
# builds and tests that preset. CI calls the same script through ci/ci.sh.

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage()
{
    cat <<'EOF'
Usage: build.sh [--platform <linux|macos|windows|wasm>] [options]

  --platform <name>   Target platform. Defaults to the current host:
                      linux -> linux, macOS -> macos, Windows (Git Bash) -> windows.
                      wasm must be requested explicitly and needs emcmake on PATH.
  --type <type> release or debug. Default: release.
  --sanitize <name>   none, address, thread or undefined. Default: none.
  --clean             Rebuild from a clean build tree.
  --no-debugger       Skip the web debugger (protobuf + uWebSockets).
  --with-debugger     Build the web debugger (the default on native platforms).
  -h, --help          Print this text and exit.

Examples:
  ./build.sh
  ./build.sh --type debug
  ./build.sh --sanitize address --no-debugger
  ./build.sh --platform wasm --type debug
EOF
}

die()
{
    printf 'build: %s\n' "$*" >&2
    exit 1
}

platform=""
build_type="release"
sanitize="none"
clean="no"
debugger="on"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform)
            shift
            [[ $# -gt 0 ]] || die "--platform requires a value"
            platform="$1"
            ;;
        --type)
            shift
            [[ $# -gt 0 ]] || die "--type requires a value"
            build_type="$1"
            ;;
        --sanitize)
            shift
            [[ $# -gt 0 ]] || die "--sanitize requires a value"
            sanitize="$1"
            ;;
        --clean)
            clean="yes"
            ;;
        --no-debugger)
            debugger="off"
            ;;
        --with-debugger)
            debugger="on"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
    shift
done

case "$build_type" in
    release|debug) ;;
    *) die "unsupported build type: $build_type" ;;
esac

case "$sanitize" in
    none|address|thread|undefined) ;;
    *) die "unsupported sanitizer: $sanitize" ;;
esac

detect_host()
{
    case "$(uname -s)" in
        Linux) echo "linux" ;;
        Darwin) echo "macos" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *) echo "" ;;
    esac
}

host="$(detect_host)"

if [[ -z "$platform" ]]; then
    [[ -n "$host" ]] || die "cannot infer a platform on this host; pass --platform"
    platform="$host"
    printf 'build: no --platform given; defaulting to %s\n' "$platform" >&2
fi

case "$platform" in
    linux)
        [[ "$host" == "linux" ]] || die "linux builds must run on Linux (host: ${host:-unknown})"
        preset_prefix="linux_x64"
        ;;
    macos)
        [[ "$host" == "macos" ]] || die "macos builds must run on macOS (host: ${host:-unknown})"
        preset_prefix="macOS_arm64"
        ;;
    windows)
        [[ "$host" == "windows" ]] || die "windows builds must run under Git Bash on Windows (host: ${host:-unknown})"
        preset_prefix="windows_x64"
        ;;
    wasm)
        preset_prefix="wasm32"
        ;;
    *)
        die "unknown platform: $platform (expected linux, macos, windows or wasm)"
        ;;
esac

if [[ "$platform" == "wasm" ]]; then
    debugger="off"
    if ! command -v emcmake >/dev/null 2>&1; then
        emsdk_dir="${EMSDK_DIR:-$HOME/emsdk}"
        if [[ -f "$emsdk_dir/emsdk_env.sh" ]]; then
            # shellcheck disable=SC1091
            source "$emsdk_dir/emsdk_env.sh" >/dev/null 2>&1
        fi
    fi
    command -v emcmake >/dev/null 2>&1 \
        || die "emcmake not found; run ci/install-emsdk.sh first"
fi

configure_preset="${preset_prefix}_${build_type}"
build_preset="$configure_preset"
test_preset="$configure_preset"

cache_args=(
    "-DCLOCKR_SANITIZE=$sanitize"
    "-DCLOCKR_BUILD_DEBUGGER=$([[ "$debugger" == "on" ]] && echo ON || echo OFF)"
)

printf 'build: platform=%s preset=%s sanitizer=%s debugger=%s\n' \
    "$platform" "$configure_preset" "$sanitize" "$debugger"

cd "$ROOT_DIR"

if [[ "$platform" == "wasm" ]]; then
    emcmake cmake --preset "$configure_preset" "${cache_args[@]}"
else
    cmake --preset "$configure_preset" "${cache_args[@]}"
fi

if [[ "$clean" == "yes" ]]; then
    cmake --build --preset "$build_preset" --clean-first
else
    cmake --build --preset "$build_preset"
fi

ctest --preset "$test_preset"
