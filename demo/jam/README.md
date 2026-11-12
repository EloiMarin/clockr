# branchable live jam

`demo/jam` is the proof consumer for `clockr::replay`: a fixed musical network
whose editable state is data, recorded as a boundary journal and replayed into
fresh programs for branching and A/B comparison.

Design and plan: `doc/explanation/branchable-live-jam.md` and
`doc/explanation/branchable-live-jam-plan.md`.

## Contents

- `jam/` — the bounded score/pattern/patch model, the fixed network
  (`network.hpp`), the shared large-seed `scan` helper, the session/action log,
  and deterministic metrics.
- `native/` — the headless conductor core: a deterministic tick schedule, an
  SPSC edit queue applied only at conductor boundaries, a bounded render ring
  for a device callback, and a `replay::session` journal. Build with
  `-DCLOCKR_BUILD_JAM_NATIVE=ON`; run `jam_conductor --selftest`.
- `shell/` — the single-worker Emscripten shell and page (score grid,
  transport, scenario export/import, branch replay, raw-vs-candidate audition).
- `tests/` — score/patch laws, network goldens and parity, voice/capacity
  behaviour, session validation, metrics, and the jam's replay conformance
  suite.

The jam reuses `daw_demo`'s pure transport functions and wire structs; it does
not duplicate the scheduling math.

## Run the page locally

Build the wasm shell (Emscripten required):

```
./build.sh --platform wasm --type release
```

Serve the staged page and open the printed URL:

```
node demo/jam/shell/serve.mjs _build/wasm32_release/demo/jam/shell 8000
```

The page edits the fixed score, records a boundary journal, exports/imports
it, forks a patch revision, and auditions raw vs candidate. It runs in a
single worker; no debugger server or network is involved.

For the headless conductor:

```
cmake -S . -B _build/jam-native -G Ninja \
  -DCLOCKR_BUILD_DEMO=ON -DCLOCKR_BUILD_JAM=ON -DCLOCKR_BUILD_JAM_NATIVE=ON
cmake --build _build/jam-native --target jam_conductor
_build/jam-native/demo/jam/native/jam_conductor --selftest
```

