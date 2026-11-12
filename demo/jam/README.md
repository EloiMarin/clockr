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
