#!/usr/bin/env bash
# Throwaway animation lab: configure/build the wasm module and stage the page.
# Run from anywhere; the script cd's to its own directory.

set -euo pipefail

cd "$(dirname "$0")"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "[lab] emcmake not found. Activate Emscripten, or run:" >&2
    echo "      nix shell nixpkgs#emscripten -c ./build.sh" >&2
    exit 1
fi

emcmake cmake -S . -B _build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _build --target lab_shell

rm -rf _site
mkdir -p _site/web

cp index.html lab.css lab.js lab-common.js _site/

for file in web/*.js; do
    [ -e "$file" ] && cp "$file" _site/web/
done

cp _build/lab_shell.js _build/lab_shell.wasm _site/

echo "[lab] staged _site/ — run: node serve.mjs"
