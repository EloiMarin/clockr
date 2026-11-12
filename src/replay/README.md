# clockr::replay

`clockr::replay` is an **opt-in** module (target `clockr::replay`) for
re-executing a recorded program with the ordinary engine. It builds only on
`clockr::kernel`; link it explicitly, because the umbrella `clockr` target does
not include it.

Replay is not part of the engine's core proposition. It consumes the kernel's
invariants — fresh-program construction, state-as-data, boundary-anchored
clocks, explicit crossings, and determinism — and adds no evaluation semantics.

```
scenario (boundary journal) ──▶ replayer ──▶ fresh program ──▶ engine::step
                                           └─▶ run ──▶ compare
```

## Contents

- `identity.hpp` — run, branch, entry, and logical-step ids. A logical step is
  `(clock, ordinal)`; epochs are display metadata only.
- `scenario.hpp` — immutable scenarios, the `scenario_builder`, branch edits,
  and `apply_branch`.
- `value_codec.hpp` — scalar and host-registered user-value encoding. A journal
  stores bytes, never a pool cell owned by a live or dead program.
- `session.hpp` — records effective input sets and region steps while applying
  them to a live program.
- `factory.hpp`, `native.hpp` — the host `program_factory` and
  `native::recipe_factory<Recipe>`.
- `run.hpp`, `replayer.hpp` — one replay execution and the fresh-program
  replayer (prefix replay and branch replay).
- `compare.hpp` — semantic run comparison by step/port/output, ignoring
  durations, sequence numbers, addresses, and `user_cell*` identity.

## Invariants

- Replay uses `engine::step` into a fresh `program`; there is no second
  evaluator.
- The lossy observer trace is never a journal and is never replayed.
- An incomplete journal may replay, but its result must never be labelled exact.
- Journal user values are decoded into the target program's region pools; a
  journal never carries a live cell.
