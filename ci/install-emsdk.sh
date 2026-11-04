#!/usr/bin/env bash
#
# Install (or update) the Emscripten SDK used by the wasm CI jobs.
#
# The SDK is installed to ${EMSDK_DIR:-$HOME/emsdk}, which ci/ci.sh looks for
# when `emcmake` is not already on PATH. This split keeps the install step
# independent of the shell that runs the build, so it works unchanged on
# GitHub Actions, Forgejo Actions and Woodpecker.
#
# Pin a version with EMSDK_VERSION=<tag> (e.g. `3.1.74`). Defaults to `latest`
# because the project does not currently pin an Emscripten release; set it in
# CI once the version used for the release is known.
#
set -euo pipefail

readonly EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
readonly EMSDK_VERSION="${EMSDK_VERSION:-latest}"

if [[ ! -d "$EMSDK_DIR/.git" ]]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
else
    git -C "$EMSDK_DIR" fetch --depth 1 origin
fi

"$EMSDK_DIR/emsdk" install "$EMSDK_VERSION"
"$EMSDK_DIR/emsdk" activate "$EMSDK_VERSION"

printf 'ci: emscripten %s installed in %s\n' "$EMSDK_VERSION" "$EMSDK_DIR"
