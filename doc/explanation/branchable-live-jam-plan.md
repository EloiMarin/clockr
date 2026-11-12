# Branchable live jam — implementation plan

*Companion to [`branchable-live-jam.md`](branchable-live-jam.md). This is a
working plan, not an API specification: type and file names below are
proposals to settle during implementation.*

**Status:** partially implemented. A0–A5, A6 (native adapter and
native/wasm compilation), B0–B4, C's metrics, and a first B5/B6 slice are in
the source tree and tested. The replay substrate is an **opt-in**
`clockr::replay` target, not part of the umbrella `clockr` interface, and the
DAW is its non-jam conformance consumer. Still open: the `clockr.replay.v1`
wire/protocol adapters, the shared collaboration gateway (B7), the MCP tool
pack, checkpoints, and website `/jam/` staging. The design intent is unchanged:
the jam is an example application that makes clockr's properties audible, not a
redefinition of clockr's core proposition (real-time-safe concurrent
coordination in audio software).

## 0. What this plan is for

Two outcomes, in priority order:

1. **A reusable replay substrate** in the library: scenario/run/branch
   identity, boundary journaling, fresh-program replay, typed value codecs,
   and semantic comparison. Another clockr application must be able to use it
   without implementing a second evaluator or application-specific state
   restoration.
2. **A jam** that consumes that substrate: a fixed musical network, a
   high-level editor, branches, A/B audition, and agent-reviewed alternatives.

The substrate is the product; the jam is the proof that it is reusable. Every
core change below is justified by a non-jam consumer (the existing DAW first,
the concurrent workbench second).

Definition of done for this plan:

- a recorded DAW or jam session replays into a fresh `kernel::program` through
  the ordinary `engine::step` path and reproduces semantic outputs;
- a branch replays the unchanged prefix and executes an edited suffix without
  mutating the original scenario;
- the same scenario can be replayed natively and in wasm with a shared
  conformance fixture;
- the jam can record a shared performance, fork it, audition the result, and
  let an agent propose a validated patch through MCP;
- none of this allocates, blocks, or rebuilds a program on the audio thread.

## 1. Baseline

Already present and reusable:

| Area | What exists |
| --- | --- |
| `kernel::program` | Owning (recipe) and borrowing (descriptor) construction; per-region runtimes; `set`/`get` by name and index; state-as-data (`delay_states`, region epochs); pools and deferred reclamation. |
| `kernel::engine` | Non-virtual `step(program, clock)`; `step_hook` (`before_step`/`after_step`) at the tick boundary; allocation-free evaluation; region overlap guard. |
| Trace | `debug_engine` emits `trace_event`s to a `trace_sink`; `trace_recorder` in `tools/web-debugger` is a bounded, lossy, lock-free live consumer. |
| Structure | `shell::compile` → `descriptor_storage`; `kernel::descriptor_view`; named ports; `user_type`/`describe<T>`; inline or pooled user values. |
| Injection | `debug::command_hook` drains per-region SPSC command/reply queues at the boundary; `SetInput`/`ReadInput`/`ReadOutput` over `clockr.debug.v1`. |
| Demo | DAW P1 network + transport; native host with ui/audio threads; wasm shell; website staging. |
| Clients | `clockr-debug-client` (protocol, history replay of traces, injection); `clockr-debug-mcp` (stdio MCP). |

Missing, and the reason for this plan:

- no scenario/run/branch identity or boundary journal;
- no typed value codec or runtime materialization for user structs (the debug
  schema is descriptive only);
- no fresh-program replay factory or replayer;
- no semantic run/trace comparator;
- no native/wasm replay conformance path;
- the DAW score is captured in scheduler closures, so it is not editable data;
- no jam network, editor, collaboration gateway, or jam MCP tools.

The existing trace recorder is **not** a replay journal. It drops events by
design and records observations after evaluation. Replay must re-execute; it
must not reorder `TraceEvent`s.

## 2. Decisions to lock before coding

| Id | Decision | Rationale |
| --- | --- | --- |
| R1 | Add a new `clockr::replay` module under `include/clockr/replay/` and `src/replay/`. At the source level it uses only `clockr::kernel`; the kernel's existing transitive `conc`/`mem` and shell-header coupling is unchanged, and the module adds no dependency on the debugger tools. | Keeps music and the debugger out of the kernel; avoids deepening the existing kernel→shell layering smell. Host factories supply recipes/descriptors. |
| R2 | A scenario is an immutable ordered log of boundary operations; a run is one execution; a branch is a revision with a parent and a cut entry. | Mirrors the design's "original run is immutable" rule and makes comparison well-defined. |
| R3 | Identity is `(run_id, entry_id)` for the log and `(run_id, clock, ordinal)` for region steps. Epochs are display/resampling metadata only. | A throwing tick reuses an attempted epoch; wall-clock and drain order are not semantic IDs. |
| R4 | Record explicit boundary journals first (`session` API), arbitrary live capture second. | The jam and wasm use complete-by-construction journals; faithful native capture needs drain provenance and is the companion workbench. |
| R5 | Journal values are encoded by host-registered codecs keyed by `user_type::id`/name; scalars are direct. | `value` may own a pool cell; the journal must not retain a cell owned by a dead program. The debug schema is not a constructor. |
| R6 | Replay uses the ordinary `engine::step` path into a fresh `program` built by a host `program_factory`. | This is the claim under test. A second evaluator or a JS scheduler copy is out of scope and forbidden. |
| R7 | Comparison is semantic: by step/node/output; ignore durations, sequence, addresses, and `user_cell*` identity. | Makes replay reproducible across runs and adapters. |
| R8 | Scenario wire format is versioned (`clockr.replay.v1`) and owned by the tools, not by core. | Core stays protobuf-free; the debugger/client gain messages additively. |
| R9 | The jam keeps a fixed network; edits are data (`ScorePatch`), not topology changes. | Matches decision 1 in the design doc and keeps replay within the fixed-descriptor contract. |

## 3. Workstream map

```text
A. core replay substrate ──────────────┐
   A0 module  A1 identity  A2 codecs    │
   A3 session A4 replayer  A5 diff      │
   A6 adapters/native/wasm/wire         │
                                        ▼
B. jam domain ── B0 layout  B1 score/pattern/patch
   B2 fixed network  B3 session/authoritative log
   B4 native conductor host  B5 web UI  B6 wasm shell
   B7 collaboration gateway
                                        │
C. agent/MCP ── metrics, jam tool pack ◀┘
```

A is a prerequisite for B and C. B1/B2 can proceed in parallel with A3-A5,
since they are pure data and a fixed recipe.

## 4. Workstream A — `clockr::replay`

### A0. Module, build, install, tests

Files:

```text
include/clockr/replay/
  fwd.hpp            forward declarations and ids
  identity.hpp       run_id, branch_id, entry_id, region_step_id
  value_codec.hpp    encoded_value, codec_registry
  scenario.hpp       scenario_entry, scenario, builder, branch_edit
  session.hpp        boundary recorder
  factory.hpp        program_factory
  run.hpp            step_record, run, outcome
  replayer.hpp       replayer, replay_options
  compare.hpp        run_diff, compare_options
src/replay/
  scenario.cpp  value_codec.cpp  session.cpp  replayer.cpp  compare.cpp
  tests/{identity,codec,session,replay,compare,adapter}.test.cpp
  CMakeLists.txt
```

CMake and packaging:

- `src/CMakeLists.txt`: `add_subdirectory(replay)`.
- `src/replay/CMakeLists.txt`: `add_library(clockr_replay STATIC ...)`, link
  `clockr_kernel`, `enable_warnings`, alias `clockr::replay`.
- Root `clockr` interface links `clockr::replay`.
- `cmake/clockr_install.cmake`: add the target, `EXPORT_NAME replay`, and the
  export list; headers install through the existing `install(DIRECTORY)`.
- Register `replay_tests` with `add_tests` (each Catch2 case becomes a CTest
  case, as elsewhere).

Exit criteria: module builds clean under the project warnings; unit tests
registered; replay itself constructs no recipes (the factory does) and adds no
dependency beyond `clockr_kernel` and its existing transitive libraries.

### A1. Identity, scenario, branch

```cpp
namespace clockr::replay {

struct run_id    { std::uint64_t value; };
struct branch_id { std::uint64_t value; };
struct entry_id  { std::uint64_t value; };
struct region_step_id { kernel::clock_tag clock; std::uint64_t ordinal; };

enum class entry_kind : std::uint8_t { init, set_input, step_region, mark };

struct scenario_entry
{
    entry_id id;
    entry_kind kind;
    kernel::clock_tag clock;      // region addressed by the entry
    std::uint64_t epoch_group;    // sets sharing this value are one input map
    std::string port;             // set_input only
    encoded_value value;          // set_input only
    bool is_event { false };      // validation metadata, not identity
    std::string note;             // optional edit-history annotation
};

struct fidelity
{
    bool complete { true };
    std::uint64_t missing_entries { 0 };
    std::string reason;
};

struct scenario
{
    run_id run;
    branch_id branch;
    std::optional<run_id> parent;
    entry_id base_entry;          // cut point for a branch
    std::vector<scenario_entry> entries;
    fidelity fidelity;
};

class scenario_builder
{
public:
    explicit scenario_builder(run_id, branch_id, std::optional<run_id> parent = {});
    void mark(std::string note);                       // musical position, review decision
    void begin_epoch(kernel::clock_tag);               // group the following sets
    void set_input(std::string_view port, encoded_value, bool is_event);
    void step_region(kernel::clock_tag);
    void mark_incomplete(std::string reason);
    scenario finish();                                  // freezes ids
};

struct branch_edit
{
    entry_id cut;                                       // replay through this entry
    std::vector<scenario_entry> inserted;               // empty = pure cut
    std::vector<entry_id> removed;                      // entries in the suffix
    std::vector<std::pair<entry_id, scenario_entry>> replaced;
};

scenario apply_branch(const scenario& base, const branch_edit&, branch_id new_branch);

}
```

Rules:

- Entry ids are monotonic per run and never reused. `epoch_group` is assigned
  by the builder; sets in one group are an unordered map (L9a). A step closes
  the group.
- A branch always names its parent run, cut entry, and new branch id. The base
  scenario is copied or shared immutably; editing never mutates it.
- A scenario with `fidelity.complete == false` may still replay, but the
  replayer reports the limitation and must never label the result "exact".
- Musical annotations (`mark`) are application metadata; the replay core treats
  them as opaque.

Tests: id monotonicity, freeze immutability, branch cut at every entry,
insert/remove/replace, same-epoch permutation equivalence, incomplete
propagation.

### A2. Values, codecs, materialization

```cpp
struct user_bytes
{
    std::uint32_t type_id;
    std::vector<std::byte> bytes;
};

struct encoded_value
{
    std::variant<std::monostate, bool, std::int64_t, double, user_bytes> payload;
};

class value_codec
{
public:
    virtual ~value_codec() = default;

    virtual bool encode(const kernel::value&, encoded_value& out,
                        std::string& error) const = 0;

    virtual bool decode(const encoded_value&, kernel::program& target,
                        kernel::clock_tag region, kernel::value& out,
                        std::string& error) const = 0;

    virtual bool equal(const kernel::value& live, const encoded_value& journal,
                       std::string& error) const = 0;
};

class codec_registry
{
public:
    template<typename T>
    void register_user(std::string name);   // uses tag_of<T>(), make_user_value<T>

    const value_codec* find(std::uint32_t type_id) const;
};
```

Behaviour:

- Scalars (`monostate`, `bool`, `int64_t`, `double`) are built in.
- `register_user<T>` supplies encode/decode/equal for a trivially copyable
  `T`. Decode constructs into the *target program's* region home via
  `kernel::make_user_value<T>(target.region(region).home, ...)`; the journal
  never carries a cell from another program.
- A user type with no codec makes the journal incomplete at the first
  occurrence; the debug schema is explicitly not used as a fallback.
- Encoding happens when the value is recorded (at `set` time), so later pool
  reclamation cannot invalidate the journal.
- `equal` compares user values semantically (field bytes/decoded values), not
  `user_cell*` identity.

Tests: scalar round-trip; POD struct round-trip; missing codec → incomplete;
decode into a fresh program; no double release; two equal structs with
different cells compare equal.

### A3. Recording: `session`

```cpp
class session
{
public:
    session(kernel::program&, kernel::engine&, codec_registry&,
            scenario_builder&);

    void mark_initialized();                       // record the init boundary
    void begin_epoch(kernel::clock_tag);
    void set(std::string_view port, kernel::value);
    template<typename T, typename... Args>
    void set(std::string_view port, Args&&... args); // owning user path

    void step(kernel::clock_tag);                  // record + engine.step
    const scenario& recorded() const;

private:
    kernel::program& m_program;
    kernel::engine& m_engine;
    codec_registry& m_codecs;
    scenario_builder& m_builder;
    bool m_initialized;
};
```

Behaviour:

- `set` encodes first, then applies through `program::set`. `step` records the
  step, calls `engine.step`, and catches an escaping exception to record
  `threw` and mark the run aborted at that entry, then rethrows (until C3
  defines an error policy).
- One session per authoritative schedule; it is not thread-safe. A session
  created after the program has ticked marks the journal incomplete ("late
  start").
- A debug-only `recording_hook : kernel::step_hook` can assert that every
  region tick was recorded; it is not required for correctness.
- The session owns no policy for live acceptance or network input; those are
  application concerns.

Tests: record a small DAW run; replay the journal in A4; a missing step is
incomplete; a throwing tick is recorded with the attempted step id; a late
session is not exact.

### A4. Replay: factory, replayer, runs

```cpp
class program_factory
{
public:
    virtual ~program_factory() = default;
    virtual std::unique_ptr<kernel::program>
    make(const scenario&, kernel::program_options) = 0;
};

struct step_record
{
    region_step_id step;
    std::uint64_t epoch;          // display metadata
    bool threw { false };
    std::vector<encoded_value> outputs;   // global output order
};

struct run
{
    run_id run;
    branch_id branch;
    std::vector<step_record> steps;
    std::vector<kernel::trace_event> trace; // optional, synchronous capture
    fidelity fidelity;
    bool aborted { false };
};

struct replay_options
{
    bool capture_outputs { true };
    bool capture_trace { false };
    bool stop_on_throw { true };
    std::size_t max_steps { 0 };            // 0 = no limit
};

class replayer
{
public:
    run replay(const scenario&, program_factory&, const codec_registry&,
               replay_options = {});
    run replay_branch(const scenario& base, const branch_edit&,
                      branch_id, program_factory&, const codec_registry&,
                      replay_options = {});
};
```

Behaviour:

- Construct a fresh `program` from the factory, replay entries in order:
  `init` marks the initial publication boundary, `set_input` decodes into the
  target region and applies, `step_region` calls `engine.step`.
- Capture outputs after each step by the program's global output order; encode
  them immediately.
- If `capture_trace`, construct a `debug_engine` with a synchronous
  `trace_sink` that appends to the run. Do **not** use the lossy live
  recorder.
- `replay_branch` replays the prefix through `base_entry`, applies the edit,
  then continues the suffix. v1 does not snapshot state; a checkpoint is a
  later optimization.
- `run.epoch` is recorded for display and resampling checks; comparison never
  uses it as identity.
- Replaying an incomplete scenario is allowed but the result reports the same
  fidelity gap and cannot be labelled exact.

Tests: unchanged replay reproduces outputs and trace semantically; branch
changes only the suffix; original run unchanged; throwing tick is represented;
late/incomplete scenario is rejected for "exact".

### A5. Semantic comparison

```cpp
struct value_mismatch
{
    std::size_t step;
    std::size_t port;
    encoded_value expected;
    encoded_value actual;
};

struct run_diff
{
    bool equal { true };
    std::size_t first_step { 0 };
    std::size_t output_mismatches { 0 };
    std::size_t trace_mismatches { 0 };
    std::vector<value_mismatch> values;
    std::string summary;
};

run_diff compare(const run& expected, const run& actual,
                 compare_options = {});
```

Rules: compare by `(region, ordinal, node, output)`; ignore `duration_ns`,
`sequence`, addresses, and `user_cell*` identity; compare user values through
the codec. Report the first divergence and counts; do not hide a size
difference.

Tests: identical runs; changed boundary event produces the expected first
divergence; timings/sequence differences do not count; user structs with equal
content compare equal.

### A6. Adapters, wire, conformance

- Native: `replay::native::recipe_factory<Recipe>` in
  `include/clockr/replay/native.hpp`; it captures the recipe by value and
  returns `std::make_unique<kernel::program>(recipe, options)`. The DAW gains a
  `daw::replay_factory()` convenience wrapper.
- Wasm: the jam shell (B6) exposes `replay`/`diff` over embind; the same core
  is compiled into `jam_shell.wasm`. No threads, single worker.
- Wire: add `tools/web-debugger/proto/clockr_replay.proto` (package
  `clockr.replay.v1`) with `Scenario`, `ScenarioEntry`, `EncodedValue`,
  `BranchEdit`, `RunDigest`, `Fidelity`. The C++ codec lives beside the debug
  server; the TypeScript codec lives in `tools/debug-client/src/replay.ts`.
  Core stays wire-free.
- Conformance: shared fixtures under `demo/jam/tests/fixtures/` (a small
  scenario and expected output digests). A native Catch2 test and a wasm node
  test replay the same fixture and compare digests. Unsupported codecs are a
  reported limitation, not a silent skip.

### A7. Relation to E1/E3/I and the live debugger

- **E1 determinism/replay check**: implement as "replay the same scenario
  twice and `compare` the runs"; reuse A4/A5 rather than a separate double-tick
  path.
- **E3 time travel**: v1 branches use prefix replay. Add a `checkpoint`
  interface (ports, delay states, links, epochs, pooled values) behind the same
  replayer API; snapshots are an optimization, never a new semantics.
- **I undo/snapshots**: reuses the E3 checkpoint path; undo is replay to an
  entry.
- **Debugger**: add explicit messages/commands (`scenario_get`,
  `scenario_branch`, `run_replay`, `run_diff`) to the debug protocol. The
  connect-time "history replay" in `clockr-debug-client` remains a resend of
  traces; program replay is a distinct operation and must not be confused with
  it.

## 5. Workstream B — jam application

### B0. Layout and build

```text
demo/jam/
  jam/
    score.hpp pattern.hpp patch.hpp scheduler.hpp voice.hpp metrics.hpp
    network.hpp session.hpp
  native/
    main.cpp host.hpp host.cpp audio.hpp audio.cpp gateway.hpp gateway.cpp
  shell/
    jam_shell.cpp index.html app.js worker.js style.css
  tests/
    score.test.cpp network.test.cpp session.test.cpp replay.test.cpp
    fixtures/
  CMakeLists.txt
```

- Root option `CLOCKR_BUILD_JAM` (default `${CLOCKR_BUILD_DEMO}`); native host
  gated like `CLOCKR_BUILD_DEMO_NATIVE`; wasm shell under `EMSCRIPTEN`.
- `jam` INTERFACE target links `clockr::shell`, `clockr::replay`, and the
  existing `daw_demo` header target for the pure transport functions. If
  depending on a demo target feels wrong, promote those pure functions to a
  small `demo/music/` header-only target and have both demos use it; do not
  duplicate scheduling math.
- Tests registered with `add_tests`; the wasm conformance test uses the same
  fixtures.

### B1. Musical model (pure, bounded, data)

```cpp
struct step { std::int8_t note; std::uint8_t velocity; bool gate; };
struct pattern { std::array<step, 32> steps; std::uint8_t length; };
struct track
{
    std::uint8_t pattern;
    std::int8_t transpose;
    std::uint8_t gain;
    bool mute;
};
struct score
{
    std::array<pattern, 16> patterns;
    std::array<track, 8> tracks;
    std::uint8_t bpm;
};

enum class patch_op : std::uint8_t
{ set_step, clear_step, transpose, rotate, mask, density, stack, sequence };

struct patch_step { patch_op op; std::uint8_t a, b, c; std::int16_t d; };
struct score_patch { std::array<patch_step, 32> ops; std::uint8_t count; };

score apply(const score&, const score_patch&);   // pure
```

- Types are trivially copyable, bounded, and described with `describe<>` so
  the debugger renders them. Large values ride the normal user-value path.
- Patch operations are data and pure functions; no closures, no per-note
  kernel nodes.
- `ScorePatch` application is off the audio thread; the new score value has
  explicit ownership through the user-value machinery.

Tests: each operation, composition, bounds, invalid indices, capacity
behavior, golden scores.

### B2. Fixed network

```cpp
namespace jam::net {

auto app(score initial);   // clockr::shell recipe

}
```

Structure (mirroring the DAW's discipline):

- ui region inputs: `patch` (queued; no coalescing), `playing`/`bpm`/`loop`
  (latest), `score_snapshot` (latest if used), `init_score` at construction.
- audio region: `score_state` (`scan`, current score), `transport_state`
  (`scan`, reused DAW timing), `events` (`lift`, pure evaluation over the
  tick interval), `voices` (`scan`, fixed voice array + stealing policy),
  `mix` (`lift`).
- back-crossings: playhead and voice pressure to ui (latest).
- Crossings are explicit at every boundary; no ambient clock.
- All editable state is an input or scan state; no captured mutable score.
- Fixed track/voice bounds; overflow is explicit (drop or steal policy), never
  an allocation on the tick path.

Tests: golden note/offset streams, patch application across epochs, same-epoch
permutation invariance, voice stealing, capacity, debug/release parity, and the
existing allocation-free evaluation check extended to the jam network.

### B3. Session and authoritative action log

```cpp
namespace jam {

struct revision { std::uint64_t value; };
struct participant { std::string id; };

class session
{
public:
    session(score initial, replay::program_factory&, replay::codec_registry&);

    bool propose(participant, revision base, score_patch,
                 std::string& error);           // validate; append
    void accept(revision);                      // record review decision
    replay::scenario const& history() const;

    replay::run render(replay::branch_id) const; // fresh-program replay
    replay::run_diff compare(replay::branch_id, replay::branch_id) const;
};

}
```

- The canonical history is a `replay::scenario` plus application metadata
  (participant, base revision, musical annotations).
- Validation: schema, bounds, base revision, and preservation constraints.
  Stale proposals are rejected or explicitly rebased; never applied silently.
- Live adoption records a new action and schedules the accepted score at the
  next musical boundary (later extension); end-of-session comparison renders
  raw and candidate independently.
- One ordered log; no CRDTs in the first version.

Tests: stale patch rejection, conflicting edits, deterministic re-render,
branch immutability, review decisions in history.

### B4. Native conductor host (v1)

To make the recorded order total and replay exact, v1 uses a **conductor**
thread, not independent ui/audio tick threads:

- the conductor advances a deterministic schedule (audio blocks and ui ticks)
  and calls `engine.step` for each region in that order;
- participant edits arrive from the gateway thread through an SPSC queue and
  are applied only at a conductor boundary through the session;
- rendered audio blocks go to a lock-free ring consumed by the miniaudio
  callback, which never touches the program;
- recording is the `replay::session` log, not the live trace;
- a `debug_engine` + trace recorder may be attached for observation, but it is
  explicitly lossy and never becomes the journal.

A later native variant may let the audio callback tick directly and record
per-region logs merged by a known order; that is the companion workbench's
problem, not v1's.

Tests: deterministic conductor schedule; no ring underrun in a synthetic
run; recorded scenario replays to the same digest; callback never allocates or
blocks.

### B5. Web UI (single-host Phase C)

- Page under `demo/jam/shell/` (served locally) and staged at `/jam/` by the
  website build.
- High-level editor: tracks, patterns, steps/notes, velocity, transpose, mute;
  transport; a piano-roll/timeline. Editing lowered kernel nodes is out of
  scope.
- Two timelines, kept distinct: **edit history** (action log, revisions,
  branch points, review decisions) and **musical time** (bars, beats, loops,
  note positions). A branch is created at an entry but edited at musical
  positions; the UI maps both without collapsing them.
- Fork/edit/replay: select a history entry, apply a patch, replay the branch,
  show raw vs candidate, A/B audition.
- Debugger: reuse the existing `/debugger/` page as an iframe/companion; show
  the original and candidate graphs/traces and highlight changed
  values/events. The jam never lowers kernel nodes into the live-coding
  language.
- Tests: JS smoke tests (bootstrap, worker, scenario export/import, branch
  replay) modelled on `demo/daw/shell/*_test.js`.

### B6. Wasm shell and website

- `jam_shell.cpp` embind surface: `createScore`, `init`, `setIntents`,
  `step`, `getOutput`, `exportScenario`, `importScenario`, `replayBranch`,
  `diffRuns`. Strings/JSON at the boundary, encoded values internally.
- Single worker (no threads). The page drives it with the same scenario model
  as native; BroadcastChannel carries debug events to the local debugger.
- `tools/website/scripts/build.mjs`: add `stageJam()` mirroring `stageDaw()`
  (new `.cache/jam-wasm`, `-DCLOCKR_BUILD_JAM=ON`, target `jam_shell`), stage
  `/jam/`, and add a jam pane to the demo page. Keep the DAW and debugger
  staging intact.

### B7. Shared online canvas (Phase E)

- Start with an in-process gateway in the native host (uWebSockets is already
  fetched for the debugger) or a separate `tools/jam-server` if the host must
  stay minimal.
- Server-assigned action sequence and revision numbers; participant identity
  and permissions; optimistic concurrency; persistence of the action log.
- One authoritative renderer; participants edit high-level data; the debugger
  and MCP stay on a privileged local/server-side boundary, never exposed
  directly.
- End-of-session raw/candidate comparison; live adoption at future musical
  boundaries is a later extension.
- Tests: two clients, conflicting edits, stale proposal, reconnect, long
  history, gateway rejects unvalidated patches.

## 6. Workstream C — agent and MCP

- Deterministic metrics over a `run` and a `score`: event density,
  simultaneity, register spread, duplicate notes, unresolved sustains,
  clipping risk, voice pressure. Pure functions in `jam/metrics.hpp`.
- Integration point: extend `tools/debug-mcp` with a jam tool pack, or add
  `tools/jam-mcp` over `clockr-debug-client` plus a jam control API. Prefer a
  separate package if jam resources grow large; keep generic debugger tools as
  they are.
- Proposed tools/resources:
  - `jam_score_get`, `jam_history_get`, `jam_branch_create`;
  - `jam_patch_propose`, `jam_patch_validate`;
  - `jam_replay`, `jam_diff`, `jam_audition`;
  - resources for score/pattern schemas and deterministic metrics.
- Safety: the agent works on a branch tied to an exact base revision; it
  cannot mutate the raw run, bypass validation, or apply a stale proposal
  without review. All work happens off the audio path.
- Tests: MCP protocol/e2e with a mock client, schema and revision validation,
  stale rejection, provenance recorded in the action log.

## 7. Milestones

| Milestone | Content | Exit |
| --- | --- | --- |
| M0 | Lock R1-R9; add `clockr::replay` skeleton with tests. | Builds clean, empty suite green. |
| M1 | A1-A5 + native recipe factory. | Replay a scripted DAW scenario into a fresh program and compare runs; CTest covers it. |
| M2 | A6 native/wasm conformance + E1 on replay. | Same fixture digest on native and wasm; E1 uses `compare`. |
| M3 | B0-B2 musical data and fixed network. | Golden note streams, parity, no-alloc check. |
| M4 | B3-B6 session, conductor host, web UI, wasm. | Single-host fork/replay/A-B demo; recorded session replays. |
| M5 | C agent/MCP pack. | Agent proposes a validated branch; human review recorded. |
| M6 | B7 shared canvas. | Two participants, conflicts, stale proposal, long session. |
| Later | E3/I checkpoints; arbitrary live capture; remote audio. | Not prerequisites for the jam. |

M1/M2 can be developed against the DAW before the jam exists; that is the
cleanest proof that the substrate is generic.

**Current state.** M0–M3 are landed, with M1/M2 proven on both the DAW
(`demo/daw/tests/replay.test.cpp`) and the jam
(`demo/jam/tests/replay.test.cpp`). M4 is landed in its headless/core form:
`jam::session` + metrics, the native conductor (`demo/jam/native`), the wasm
shell and page (`demo/jam/shell`), and browser smoke tests. M4's website
staging and the full debugger companion remain open. M5's metrics exist, but
the MCP tool pack does not; M6 (shared canvas) is not started.

## 8. Test matrix

| Layer | Where | What |
| --- | --- | --- |
| Unit | `src/replay/tests/` | identity, codecs, session, replayer, compare, incomplete/throwing cases. |
| Unit | `demo/jam/tests/` | score/patch laws, network goldens, voice policy, capacity, parity, no-alloc. |
| Integration | native + wasm fixtures | scenario replay digest agreement; explicit limitation reporting. |
| Integration | native host | conductor determinism, ring behaviour, callback no-alloc/no-block. |
| E2E | browser/node | fork, edit, replay, A/B; gateway conflict/reconnect; MCP tool flow. |
| CI | `./ci/ci.sh <preset>` | host presets, sanitizers, wasm build; TS node tests unchanged. |

## 9. Risks and mitigations

- **Codec/materialization is the real hard part.** Start with scalars and
  trivially copyable PODs; mark everything else incomplete. Do not grow a
  schema-based constructor by accident.
- **Cross-thread ordering.** The v1 conductor host avoids it by construction.
  Arbitrary live capture is deliberately deferred to the companion workbench.
- **Memory.** Field user structs can be large; size `region_pool_bytes` for the
  jam with headroom, and keep replay construction off the audio path.
- **Wasm/native divergence.** Shared fixtures and digests from day one; report
  fidelity rather than hiding differences.
- **Trace vs journal confusion.** Keep the lossy recorder and the journal
  separate in code, names, and protocol. Never replay a trace.
- **Epoch reuse on throw.** Use logical step ids; keep epochs for display.
- **Scope creep.** No structural live coding, no CRDT, no remote synchronized
  audio in the first version (design doc §12).

## 10. Documentation and deliverables

- Update `branchable-live-jam.md` §10 to point at this plan (already linked
  from the status block).
- Update the root `README.md` roadmap when M1/M2 land (E1/E3/I status).
- Update `GLOSSARY.md` with the new vocabulary: scenario, run, branch,
  boundary journal, logical step, fidelity. Keep one canonical term per
  concept; add aliases only where the code uses a different spelling.
- Update `doc/README.md` if the plan becomes a separate reading entry.
- Acceptance: the demo acceptance criteria in the design doc §11 map to the
  milestones above; do not mark a milestone done until its acceptance test is
  live.
