#!/usr/bin/env bash
#
# Forge-agnostic CI entry point.
#
# This script maps a CMake preset name to the unified build entry point in
# build.sh. Forge-specific workflow files (GitHub Actions, Forgejo Actions,
# Woodpecker, ...) should only install toolchains and then call this script,
# so migrating the repository between forges does not mean re-implementing
# the build.
#
# Usage:
#   ci/ci.sh <preset> [--sanitize address|thread|undefined|none]
#                     [--no-debugger] [--with-debugger]
#
# Examples:
#   ci/ci.sh linux_x64_debug
#   ci/ci.sh linux_x64_release --no-debugger
#   ci/ci.sh linux_x64_debug --sanitize address --no-debugger
#   ci/ci.sh wasm32_debug --no-debugger     # needs ci/install-emsdk.sh first
#
set -euo pipefail

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"

usage()
{
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-1}"
}

die()
{
    printf 'ci: %s\n' "$*" >&2
    exit 1
}

preset=""
sanitizer="none"
debugger_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sanitize)
            shift
            [[ $# -gt 0 ]] || die "--sanitize requires a value"
            sanitizer="$1"
            ;;
        --no-debugger)
            debugger_args=(--no-debugger)
            ;;
        --with-debugger)
            debugger_args=(--with-debugger)
            ;;
        -h|--help)
            usage 0
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            [[ -z "$preset" ]] || die "only one preset may be given"
            preset="$1"
            ;;
    esac
    shift
done

[[ -n "$preset" ]] || usage 1

case "$preset" in
    linux_x64_debug|linux_x64_release)
        platform="linux"
        ;;
    macOS_arm64_debug|macOS_arm64_release)
        platform="macos"
        ;;
    windows_x64_debug|windows_x64_release)
        platform="windows"
        ;;
    wasm32_debug|wasm32_release)
        platform="wasm"
        ;;
    *)
        die "unknown preset: $preset"
        ;;
esac

build_type="${preset##*_}"
case "$build_type" in
    debug|release) ;;
    *) die "unknown build type in preset: $preset" ;;
esac

exec "${ROOT_DIR}/build.sh" \
    --platform "$platform" \
    --type "$build_type" \
    --sanitize "$sanitizer" \
    ${debugger_args[@]+"${debugger_args[@]}"}
