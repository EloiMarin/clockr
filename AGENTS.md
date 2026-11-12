# AGENTS.md

`clockr`: C++20 library implementing a real-time-safe, thread-aware FRP
engine for audio apps. Cross-platform by intent (Linux, macOS, Windows;
mobile a future target); namespace `clockr`. Pure library — the executables
are the tests and host examples (`tools/web-debugger` adds the optional debug-UI
server + demo binary, native-only and off in wasm; its web UI also speaks an
in-browser BroadcastChannel transport fed by the wasm demo; the TypeScript
debugging packages live beside it under `tools/`).

This file is the **agent-facing** guide: the architecture map, the invariants
that must survive any change, and the session protocol. Build, test,
sanitizer, style and commit instructions live in `CONTRIBUTING.md` — read it
before running anything. CI is documented in `doc/how-to/ci.md`.

## Read first — design docs

The architecture is documented, not implicit. `doc/README.md` is the reading
map; consult it for entry paths. Read these before touching anything
structural:

- `GLOSSARY.md` (repo root) — **the vocabulary**: one canonical term per
  concept, with the alias index for names still in the code and the rejected
  terms. Consult it before naming or documenting any construct — it is
  normative.
- `doc/learn/layering.md` — **the "how to think about it"**: a mental-model
  guide to the abstraction layers (typed shell ↔ erased kernel), written for
  newcomers to FRP and type erasure.
- `doc/explanation/frp-decisions.md` — **the "why"**: FRP flavour decision
  (clocked synchronous FRP — clocks and policies) and client-API shape (Shape
  C: signal networks as composable values). Options considered and rejected
  are recorded there.
- `doc/explanation/operator-sufficiency.md` — what the operator set covers,
  what it deliberately does not, and why.
- `doc/reference/denotational-semantics.md` — **the "what it means"**:
  denotational semantics of every construct, laws L1–L9, acceptance criteria.
  Uses **[today]** / **[planned]** markers — keep them in sync with the code.
- `doc/learn/semantics.md` and `doc/learn/your-first-network.md`
  — newcomer-facing walkthroughs.
- the roadmap section of the root `README.md` — **the "where we are"**: what
  has landed, remaining work in phases A–I, known refinements. Update when a
  phase lands.
- `doc/archive/` — delivered talks and historical artifacts (currently the
  ADC 2026 talk page and slides). For the "how we got here", not for current
  status.

## Pedagogical goal

This repository doubles as an **introduction to FRP** for developers who
aren't familiar with it: the docs (`doc/learn/layering.md`,
`doc/reference/denotational-semantics.md`, `doc/explanation/frp-decisions.md`)
*teach*, they do not
just record. Preserve that voice: explain the "why", keep the concrete
examples, and prefer prose that carries a reader through the reasoning. When
editing existing docs or writing new ones, treat the reader as a C++
developer who has never seen FRP — do not assume the vocabulary.

## Environment and workflow

See `CONTRIBUTING.md` for prerequisites, the `build.sh` entry point, the
`ci/ci.sh` wrapper, the CMake presets, sanitizers, wasm, the debugger and
test selection. The short version: clang is the toolchain, `direnv allow`
loads the Nix shell, `./build.sh` builds the host platform, and
`./ci/ci.sh <preset>` reproduces CI locally. `flake.nix` pins that shell for
CI (`nix develop .#ci`); Linux, WebAssembly and website jobs use it, while
macOS and Windows keep their native toolchains.

## Layout

Public headers live in `include/clockr/`; `src/` holds the matching
`.cpp` sources plus each module's `tests/`:

- `mem/` — arena + pool allocators, links TLSF (upstream has no build system;
  `cmake/tlsf.cmake` only populates the sources, which are compiled straight
  into `clockr_mem`)
- `conc/` — header-only (INTERFACE lib): `link`, `spsc`, `slot`,
  `double_buffer`, `mpsc_stack`; `policy` is the crossing policy enum
- `kernel/` — the erased execution machinery. Graph types: nodes (`node` /
  `node_descriptor`), edges, `value`, `topology`, per-node `node_label`, and
  the user-value machinery (`value_home`, `user_cell`, `user_type`/
  `describe<T>`). Execution: `program` (the single execution plane) plus
  `engine` / `debug_engine` / `release_engine` and the `trace` stream. A pure
  description plus its interpreter; nothing here knows C++ user types beyond
  `value`.
- `shell/` — typed composition layer over `kernel` (Shape C): `recipe<A,B>`
   from operators (`lift` (N-ary over pair wires)/`operator>>`/`input`/
   `output`/`share`/`parallel`/`zip`/`delay`/`scan`/`stepper`), stored as a
   **by-value `shell::node` arena** (a flat `std::array`, children referenced
   by `node_ref` index) so a recipe is a compile-time constant. `build()` lowers a recipe to the structural
   `kernel::graph` (topology analysis); `compile()` (constexpr) produces a
   `descriptor_storage`; `kernel::program` either owns (built from a recipe,
   closures cloned) or borrows (from a `descriptor_view`) — a single execution
   plane since the runtime-graph ditch (the root `README.md` roadmap section,
   D1). All three walk the arena into a `draft` (share memoization keyed on the
   source-location-minted `m_id`, `(m_id, clock)`). Clocks are anchored at
   boundaries: `input`/`output` carry a `clock_tag`, interior nodes inherit the
   clock of the nearest upstream anchor, and there is no ambient tag.
   `instantiate` returns an output *span* (multi-output fragments like `zip`,
   law L8). Depends only on `kernel` (the reverse is not true in practice:
   `kernel::program`'s recipe constructor takes a shell recipe, so the *kernel*
   includes *shell* headers — a known layering smell, kept until phase F3; see
   the roadmap section of the root `README.md`).
- `replay/` — an **opt-in** module built only on `kernel` (link
  `clockr::replay` explicitly; the umbrella `clockr` target does not include
  it). Scenario/run/branch identity, boundary journals (`session`), host
  codecs (`value_codec`/`codec_registry`), fresh-program replay (`replayer`/
  `program_factory`), and semantic comparison (`compare`). Replay re-executes
  through `engine::step`; the lossy observer trace is never a journal and is
  never replayed.

`doc/` — the design documents, organised by genre (reading map:
`doc/README.md`): `doc/learn/` (newcomer walkthroughs: `layering.md`),
`doc/explanation/` (rationale: `frp-decisions.md`,
`operator-sufficiency.md`, and the branchable-live-jam design/plan), `doc/reference/`
(`denotational-semantics.md`), `doc/how-to/` (`consuming.md`, `ci.md`),
`doc/examples/` (small host programs), and `doc/archive/` (delivered talks and
history). The canonical
`GLOSSARY.md` lives at the repo root.

`tools/web-debugger/` (repo root) — the live debugging web UI (design and usage:
`tools/web-debugger/README.md`): the server is native-only,
`CLOCKR_BUILD_DEBUGGER`-gated (fetches protobuf + uWebSockets), while the page
also has a `?local=1` BroadcastChannel transport for the wasm demo. `server/` holds the lock-free
`trace_recorder`, the protobuf codec, the WebSocket/static-file server, and
`commands.hpp` — the write-back (injection) path: per-(example, region)
command/reply queues plus `debug::command_hook`, the `kernel::step_hook`
adapter attached once per example (the tick loops stay
`engine.step(program, clock)`);
`proto/clockr_debug.proto` is the versioned wire schema; `web/` is the page
(a minimal proto3 reader, an SVG graph with a pan/zoom/drag viewport, and the
timeline scrubber) with the layout engine ELK vendored under `web/vendor/`
(EPL-2.0; the repository `LICENSE.md` carries the notice). `main.cpp` builds
`debugger_demo`, whose network exercises all three regions, both policies,
state, and a throwing node. Tests register as `debugger_tests` plus the
node-run `debugger.schema.smoke` and `debugger.layout.smoke`.

The independent TypeScript packages under `tools/`: `debug-client/`
(`clockr-debug-client` — the protocol client with history replay and
injection), `debug-mcp/` (`clockr-debug-mcp` — the stdio MCP server over it),
and `debug-dsh-plugin/` (a **preview** Cordis adapter over the same client:
source only, not published, not booted in a real dsh process — dsh is
developer-preview). The agent workflow is `doc/how-to/agent-live-debug.md`.
Each node package runs its own `node test/*.test.ts` harness (no install).

`tools/website/` (repo root) — the landing page, the demo page (DAWmulator first, debugger on a switch, and a runtime selector between the in-browser wasm pair and an externally hosted live pair), and the published design docs (design and usage: `tools/website/README.md`). The static site is staged by Node (landing page + `/demo/` page + `doc/`/`GLOSSARY.md` rendered under `/docs`) and also bundles a wasm animation built at site-build time with Emscripten: `tools/website/animation/` is the standalone Emscripten shell for the Cursor Clocks recipe, and `tools/website/site/animation/` holds the page-side renderers (cursor dial + ambient field, both behind the content). When Emscripten is available it also builds the DAWmulator shell and stages `/daw/` plus the debugger UI at `/debugger/`; the two talk over a same-origin BroadcastChannel, so the demo works without the live host. The build skips the animation with a warning when `emcmake` is unavailable. `site.config.json` holds the public name/copy and the source URL/blob prefix, so the site is not tied to GitHub; the build is forge-neutral. Hosting and deployment configuration are intentionally kept out of this repository. `tools/website/lab/` is the throwaway wasm animation lab with its own Emscripten build and `HANDOVER.md`; its Cursor Clocks sketch was promoted to the landing page, and the rest of its sketches are built and staged as the site's 404 gallery.

`demo/jam/` (repo root) — the branchable-live-jam example (design/plan:
`doc/explanation/branchable-live-jam*.md`): a bounded score/pattern/patch
model (`jam/score.hpp`, `pattern.hpp`, `patch.hpp`), a fixed editable network
(`jam/network.hpp`), a score `scan` seeded through `jam/seeded_scan.hpp`, a
session/action log (`jam/session.hpp`), deterministic metrics
(`jam/metrics.hpp`), a headless conductor core with explicit SPSC edit/render
queues (`native/`), and an Emscripten shell + page (`shell/`). Tests are
`jam_tests` plus the native selftest and node shell smokes; the jam reuses
`daw_demo`'s pure transport functions.

Top-level `clockr` umbrella target (aliases `clockr::mem|conc|kernel|shell`).
`clockr::replay` is deliberately opt-in and not part of the umbrella.
First-party headers are quoted and namespaced (`#include
"clockr/kernel/graph.hpp"`, `#include "clockr/kernel/debug_engine.hpp"`,
`#include "clockr/shell/recipe.hpp"`); angle includes are reserved for
third-party and system headers. The include root is `include/` in the source
tree (`${CMAKE_INSTALL_INCLUDEDIR}` once installed).

## Architecture invariants

- `kernel::value` is the only type flowing at runtime: the scalars
  (monostate/bool/int64/double) plus user structs, either inline (small
  trivially-copyable PODs) or as a ref-counted pointer to a pool cell
  (`user_cell`). It stays trivially copyable and 32 bytes; a struct's C++
  type is checked in the shell at composition time and (for the strict read)
  by a runtime type tag, never by the kernel.
- User cells are allocated from the owning region's `mem::pool` and are
  ref-counted explicitly at the points a value escapes a tick (ports, delay
  state, links, end of tick). The lifetime machinery is not RAII in the
  variant — that is what keeps `value` trivially copyable. User-typed
  `delay`/`scan` seeds are materialized at program construction
  (`node_descriptor::materialize_fn`), never on a ticking thread. The debug
  schema (`describe<T>`) rides a per-type `user_type` vtable collected into
  `program::types()`, which `program::init` expands with nested field types.
- `kernel::graph` (structural form) and `descriptor_view` are pure
  descriptions: cheap to build, trivial to copy, safe to hold `const&`.
  Engines consume either through the `graph_c` concept; two engines must be
  able to evaluate the same program (the parity tests do this).
- `debug_engine` and `release_engine` must always produce identical outputs
  (law L5 — parity test guards it).
- Determinism: topological evaluation order (Kahn, lowest-ready-id-first);
  same-step values for all consumers (laws L1/L2). Any evaluation change must
  keep the laws in `doc/reference/denotational-semantics.md` true.
- One step at a time per region: a region may move between threads over time
  (a thread pool stepping `worker`), but two threads must never step it
  concurrently. `region_runtime::in_step` asserts this in debug builds
  (`engine`'s `region_step_guard`); release builds compile the check out.
- **The tick boundary is a kernel seam, not a host wrapper.**
  `engine::step` brackets an optional attached `kernel::step_hook`
  (`engine::set_step_hook`, non-owning): `before_step` runs first, before the
  affinity guard and reclamation, so a between-tick write is state the step
  consumes; `after_step` runs exactly once — after the published epoch, or on
  the unwind path with `threw` and the attempted epoch. Hooks must be cheap
  and must not throw (a throw would compound an in-flight one); attaching
  none costs two branches and no virtual call. The call site stays
  `engine.step(program, clock)` for every engine type — `debug_engine` vs
  `release_engine` is a construction-time choice — so injection happens at
  the boundary, never during evaluation (L1/L2 unchanged). Layering: the
  hook interface is `kernel`; the command queue behind `debug::command_hook`
  is `tools/web-debugger`.
- No heap allocation on the evaluate path is the phase-C3 requirement, pinned
  by a live test (`kernel | Evaluate performs no heap allocations`,
  `src/kernel/tests/engine.test.cpp`), not by roadmap text.
- Owning (`program{recipe}`) and borrowing (`program{descriptor_storage}`)
  constructions of the same network must evaluate identically (the parity
  invariant, GLOSSARY.md `program`); the engine reads the descriptor
  through spans and is producer-agnostic. `shell::recipe`s are a by-value
  `shell::node` arena (GLOSSARY.md `recipe`), not a `shared_ptr`
  tree.
- Replay is **opt-in and non-core**: `clockr::replay` consumes the engine's
  invariants but adds no evaluation semantics. It re-executes a boundary
  journal into a fresh `program` through ordinary `engine::step`; it never
  replays or reorders the lossy observer trace.
- Design decisions live in `doc/`, not in commit messages: when changing
  architecture or temporal/state semantics, update the relevant doc and this
  file.

## Session protocol

- **Pin the baseline commit first.** The first action of every agentic session
  is `git rev-parse HEAD`; store the hash as session context and carry it
  explicitly across compaction (it belongs in the compaction summary, not in
  recollection).
- **Re-check it on resume.** After compaction, or whenever a session resumes,
  compare `git rev-parse HEAD` against the stored baseline. A mismatch means
  the repository moved underneath the session: run
  `git log --oneline <baseline>..HEAD` and `git status --short`, and
  re-validate any assumption the old context rests on before proceeding.

## Notes for future agents

Findings that cost real investigation; keep this list alive as you learn
things.

**Meta-rule — record sparingly.** Add an insight here only when it is likely
to stay true for a long time *and* the net win, averaged over many future
sessions, beats the context it costs to carry. Do **not** record anything tied
to a development phase or the current project status: if the insight will go
stale as the implementation changes (or is plausible to change soon), or if
nobody is likely to need it again, leave it out. When in doubt, prefer the
commit message — it is free until someone goes looking.

- **CTest registers each Catch2 test case individually** (via `add_tests`),
  not one test per executable. `ctest -R "kernel/value"` selects by test name;
  `ctest -R kernel_tests` finds nothing.
- **Parenthesized aggregate initialization is rejected as a C++20 extension**
  under the project's `-Weverything -Werror`. Construct aggregate user structs
  with braced init (or braced placement-new), e.g.
  `::new (loc) T { args... }`; `std::construct_at` uses parens and trips this.
- **Two-phase lookup gotcha:** the generic `shell::from_value`'s optional
  branch calls `from_value<payload_t>` unqualified, so any overload the
  generic must recurse into (the user-struct one) has to be declared
  **before** it — a later-declared overload is invisible from the generic
  body and the generic is chosen instead, failing hard inside `std::get` (not
  a SFINAE-friendly error).
- **`concept user_value_c` excludes `std::optional`** (a class): otherwise the
  user-value `to_value`/`from_value` overloads hijack optionals and break the
  `optional<T>` port path. `kernel::is_optional` exists for exactly this.
- **`region_runtime` has a custom move ctor/assignment that does not move its
  `pool`** (`mem::pool` is non-movable): a moved-to region gets a *fresh*
  pool. Any member that points at the pool (e.g.
  `value_home home { &pool }`) must be re-pointed in move assignment, or it
  will dangle at the old pool. (The per-region pool lives in a program-owned,
  address-stable `value_home` instead — but the move-ctor lesson stands for
  anything similar.)
- **Atomicity is per-slot, not per-scan, in the `latest` link.** A ring scan
  validates each slot with its seqlock, but the producer can reuse a slot
  between two reads: a scan can miss a newer entry while an older one
  survives. `link::receive` detects a full-ring advance and reports stale
  (the caller retries) — do not add a fast path that returns the selected
  entry without that check, or two channels will tear at one cut under load.
- **Reclamation is deferred, never immediate, for pooled cells.** `release`
  hands the cell to the home's retire queue; only the home thread destroys and
  frees, and only after the grace check passes. Tests that assert a pool is
  empty after an operation must call `program::reclaim()` (or tick the owning
  region) first. While a consumer tick is preempted, its announced read window
  withholds every cell retired since it began, so a producer can exhaust the
  pool (`program_options::region_pool_bytes` is the sizing knob;
  `pool_allocator` throws `std::bad_alloc`, never returns null).
- **Inline background-job control does not work in this shell harness.** A
  `for …; do (burn) & done` followed by `kill %N`/`wait` silently fails
  (`kill %N` is not addressable in the non-interactive shell, and `wait` then
  blocks forever on the never-ending background jobs). For CPU-load/stress
  scripts, collect the PIDs from `$!` yourself and kill them in a `trap …
  EXIT`; prefer a script file over an inline command, so an abort still runs
  the cleanup.
- **The sanitizer is a CMake *cache* option (`CLOCKR_SANITIZE`), seeded from
  `$ENV{sanitize}` on the first configure.** Do not go back to reading the
  environment directly in `CMakeLists.txt`: CMake reconfigures automatically
  on any `CMakeLists.txt` edit, and with the variable unset that silently
  rebuilds an existing sanitizer tree *without* instrumentation — the tests
  then pass in a fraction of the usual time and prove nothing. `ci/ci.sh`
  passes `-DCLOCKR_SANITIZE=` so a CI build cannot hit this. When in doubt,
  `otool -L <binary> | grep clang_rt` (or `ldd` on Linux).
- **`kernel::value` must stay trivially copyable and 32 bytes** (pinned by
  `static_assert`s). The `latest` link copies its bytes word-wise with
  atomics, the debug trace travels a lock-free queue, and the engine copies
  values per fan-in edge. User structs are therefore a raw handle (inline
  bytes or a `user_cell*`), never a smart pointer; see `kernel/value.hpp`.
