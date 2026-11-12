# Portable program IR — per-user recipes for the branchable live jam

*Companion to [`branchable-live-jam.md`](branchable-live-jam.md) and its
[implementation plan](branchable-live-jam-plan.md). This is a design proposal
for the next jam direction, not an API specification.*

**Status:** planned. Nothing here is implemented. The current jam remains the
fixed-network step sequencer. This document defines the portable representation
that would let each participant own a static graph while every client runs all
peers' graphs.

**Intention.** The fixed-network jam proved that a recorded boundary journal can
be replayed into a fresh program. The next step is to let each participant
define *their own* static program, own inputs and gestures, and have every
client run every peer's program. Recipes become portable data; gestures become
recorded inputs; the arrangement becomes N programs plus one conductor.

**The constraint that shapes everything.** A `shell::recipe` is C++ templates,
lambdas, function pointers, and source-location-minted sharing. It cannot be
sent over a wire. A `kernel::descriptor_view` contains process-local `tick`,
`self`, and `materialize` pointers. The debugger's `GraphDescription` carries
topology, crossings, ports, and type schemas, but its node `label` is
descriptive (and may be empty) and it carries no executable body, constants, or
seeds. It is a skeleton, not a program. See [Why not the debug graph](#why-not-the-debug-graph).

The design decision is therefore:

> **Recipes travel as bounded, typed data; every client lowers that data into
> an ordinary `kernel::program` and runs it through `engine::step`. Arbitrary
> C++ closures never cross the wire, and there is no second evaluator.**

## 1. Goals and non-goals

**Goals.**

- Each participant authors one static graph from a portable, bounded operator
  set, with higher-level musical primitives layered over `lift`, `scan`, and
  `delay`.
- Each participant owns inputs: knobs (`latest` scalars), buttons (`queued`
  events), and sampled gestures.
- Inputs and gestures are recorded as boundary journals, replayable with the
  existing `clockr::replay` substrate.
- Every client can validate, lower, and run every peer's graph with identical
  semantics on native and wasm.
- The arrangement is multiple programs with their own journals, run in
  parallel by a deterministic conductor and mixed into one output.
- Recipe changes are explicit revisions rebuilt off the audio thread; they
  never mutate history and never rebuild inside a tick.
- The current step sequencer is the first conformance target, not a special
  case.

**Non-goals.**

- Arbitrary code execution, JIT, or shipping C++/wasm closures from peers.
- Structural graph mutation inside a tick.
- A general-purpose programming language with loops, recursion, or dynamic
  allocation in user recipes.
- Serialising arbitrary lambdas or the descriptive debug schema as a
  constructor.
- CRDT merging of graphs, synchronised remote audio, or live topology merging.
- Reusing the lossy observer trace as input history.

## 2. Two representations, one canonical wire form

The design deliberately separates authoring shape from wire shape.

### 2.1 Authoring recipe IR

The authoring layer is what a participant sees and edits. It keeps the
operators that make clockr pleasant:

```text
input<T, Clock>(name)          output<T, Clock>(name)
lift<Primitive, In..., Out>()  scan<Step, In, State>(seed)
delay<T>(seed)                 zip(a, b, ...)        // composition only
share(fragment)                parallel(a, b, ...)   // composition only
at<Clock, policy>(fragment)    // crossing annotation
```

The important change from ordinary C++ recipes is that `lift` and `scan` do
**not** carry arbitrary lambdas. They name a registered pure primitive:

```cpp
lift<"core.add", int64, int64, int64>()      // two inputs, one output
scan<"jam.patch_apply", optional<score_patch>, score>(initial_score)
```

A macro library may build higher-level primitives out of these (a scale, an
arpeggiator, an envelope). Macros expand **before** transmission; the wire
never carries a macro reference.

### 2.2 Canonical program IR

The wire and validation form is a flat, typed graph. Composition operators have
already disappeared:

- `zip` becomes ordered fan-in;
- `share` becomes fan-out (several consumers of one node);
- `parallel` becomes several outputs;
- `at` becomes a crossing record;
- `scan` becomes a `delay` node holding the seed plus an internal scanner node;
- `lift` becomes one internal node naming a primitive.

```cpp
struct port_type
{
    enum class kind : std::uint8_t { unit, boolean, integer, real, user };
    kind type;
    std::uint32_t user_type_id;      // when type == user
    bool optional;                   // event port / Maybe
};

enum class ir_node_kind : std::uint8_t { input, output, internal, delay };

struct ir_node
{
    kernel::node_id id;
    kernel::clock_tag clock;
    ir_node_kind kind;
    std::string op;                  // stable primitive id, e.g. "jam.schedule"
    std::uint32_t op_version;
    std::vector<kernel::node_id> fan_in;          // ordered
    std::vector<param> params;                    // op-specific, schema-checked
    port_type output_type;
    std::string port_name;                         // input/output only
};

struct ir_crossing
{
    kernel::node_id from;
    kernel::node_id to;
    conc::policy policy;
};

struct program_ir
{
    std::uint32_t format_version;                 // clockr.program.v1
    std::string name;
    std::uint64_t content_hash;                   // canonical hash of the above
    std::vector<ir_node> nodes;
    std::vector<kernel::node_id> outputs;
    std::vector<user_type_desc> types;            // ids, names, sizes; no constructors
};
```

`param` is a typed constant or seed. A param is either a scalar or a
`replay::encoded_value` with a registered host codec. The IR does not invent a
schema-based constructor; the replay codec registry already defines what can be
materialised.

### 2.3 Lowering

A new opt-in module, provisionally `clockr::program_ir`, owns:

- the IR types and validation;
- the primitive registry;
- a runtime descriptor builder;
- lowering into `kernel::descriptor_view` and `kernel::program`.

The builder produces the same plain data a compiled descriptor has: nodes,
labels, edges, fan-in tables, input/output id and name arrays, crossings, and
the user-type list. It owns the backing vectors and the node `self` storage, so
the borrowed `descriptor_view` stays valid for the program's life. Each
portable op supplies a `node_descriptor::tick_fn` and, when it has a user-typed
seed, a materialiser that constructs into the owning region's `value_home` at
program construction.

This is not a second evaluator. It is an IR-to-descriptor compiler; the
existing `program` and `engine` execute the result through the ordinary
`engine::step` path.

```text
authoring recipe IR
        │  expand macros, validate
        ▼
canonical program IR ──wire (clockr.program.v1)──▶ peers
        │  validate, lower
        ▼
program_storage (nodes/edges/params/self) ──▶ descriptor_view
        │
        ▼
kernel::program + engine::step
```

### 2.4 Primitive registry

Every portable node is a registered pure primitive with a stable identity and
an explicit signature.

```cpp
struct primitive_def
{
    std::string id;                  // "core.delay", "jam.schedule", ...
    std::uint32_t version;
    std::vector<port_type> inputs;
    port_type output;
    std::vector<param_schema> params;
    kernel::node_descriptor::tick_fn tick;
    kernel::node_descriptor::materialize_fn materialize;  // seeds only
};
```

Rules:

- Unknown `(id, version)` rejects the program with a visible fidelity error;
  it never silently substitutes.
- A primitive is pure, total, and allocation-bounded on the tick path.
- A step primitive for `scan` has signature
  `(state, input) -> state`; lowering inserts the seed-carrying delay node.
- The core registry contains only `input`, `output`, `delay`, and the
  machinery to call an op. Domain primitives (`daw.*`, `jam.*`) are registered
  by the application, never by the kernel.
- The step sequencer's `jam.schedule` and `jam.voice_tick` are primitive ops,
  not expression trees. This is the deliberate abstraction boundary: a raw
  `lift`/`scan` language would need loops, arrays, capacity, and stealing.

**Who may add a primitive?** Participants add *macros* — recipes over the
existing operator set — without shipping code. A genuinely new native primitive
is a registry version deployed to every client. It cannot be introduced by one
peer at runtime, because the other clients would not have an implementation.
This is the price of safe multicast, and the editor must make the distinction
visible rather than pretending that arbitrary C++ closures are shareable. A
future expression AST could widen the safe authoring surface, but it must be
bounded and versioned like any other op; it does not remove the need for a
registry.

## 3. Why not the debug graph?

`clockr.debug.v1` already sends a lossless *topology*, but its contract is
observation, not definition:

| Program IR needs | `GraphDescription` today |
|---|---|
| Normative op id | descriptive `label`, may be empty |
| Pure function body | absent |
| Constants, params, scan/delay seeds | absent |
| Port types and event-ness | ports carry name/node id only |
| Constructible user values | type schema is render-only |
| Validation and resource bounds | not an observation concern |
| Versioned execution conformance | trace protocol only |

Two `lift` nodes with different functions are indistinguishable. The debugger
may therefore consume the IR for its graph pane, but the IR is a separate,
normative schema. Keeping them distinct prevents a debugger change from
silently changing what runs.

## 4. The step sequencer as the first IR target

The current `jam::net::app` maps cleanly onto the canonical IR. Nothing in the
sequencer requires topology that the IR cannot express.

| Current node/fragment | IR form |
|---|---|
| `playing` (`ui`, latest bool) | `input` node + crossing |
| `bpm`, `loop`, `master` (`ui`, latest) | `input` nodes + crossings |
| `block` (`audio`) | `input` node |
| `patch` (`ui`, queued `optional<score_patch>`) | `input` node + queued crossing |
| transport bundle lift | `daw.transport_bundle` primitive |
| transport `scan(PosPair)` | `core.delay` seed + `daw.transport_step` scanner |
| playhead lift | `daw.playhead` primitive |
| score `scan(score)` | `core.delay` seed (encoded score) + `jam.patch_apply` scanner |
| scheduler lift | `jam.schedule` primitive |
| voice `scan(voice_state)` | `core.delay` seed (encoded voices) + `jam.voice_tick` scanner |
| mix lift | `jam.voice_mix` primitive |
| `share`/`zip`/`parallel` | fan-out, ordered fan-in, output list |
| `at<audio, latest/queued>` and `at<ui, latest>` | crossing records |
| outputs `playhead`, `events`, `voices`, `mix`, `playhead_ui` | `output` nodes |

The two hard pieces are already pure functions:

- `jam::schedule(...) -> event_batch` — a `lift`-shaped primitive.
- `jam::voice_tick(...) -> voice_state` — a `scan`-shaped step primitive.

They stay whole. Decomposing them into per-note nodes would create a dynamic
graph per note, which is exactly the complexity this design avoids.

Two current hacks disappear naturally:

- `jam/seeded_scan.hpp` exists because `shell::scan` stores its seed inline in
  a closure cell. Canonical IR `delay` params are encoded values materialised
  into the region pool at construction, so large seeds are ordinary.
- The large score and voice seeds are just params; no 16-byte inline budget is
  imposed by the IR.

### 4.1 Parity acceptance

The existing hand-written `jam::net::app` stays as the reference while the IR
path is built. The acceptance test is:

1. Build the same network twice: once from the C++ recipe, once from the
   sequencer's canonical IR.
2. Apply the same scripted boundary journal to both fresh programs.
3. Compare per-step outputs and the optional synchronous trace semantically.
4. Run with both `release_engine` and `debug_engine`.
5. Repeat on native and wasm with shared fixtures and expected digests.

Only after parity does the jam switch its construction path to the IR. The
hand-written recipe can remain as a test oracle for a while.

## 5. Per-user programs and inputs

### 5.1 Inputs

The design keeps input kinds small and explicit:

- **knob**: `double` on the ui clock, `latest` crossing.
- **button/pad**: `bool` or `optional<T>` event, `queued` crossing.
- **gesture**: an application struct sampled at a ui boundary, e.g.
  `gesture { double x, y; double pressure; bool down; }`, carried as a user
  value with a registered codec. Raw DOM/mouse events are never journaled;
  only the effective sampled value that a tick consumed is.
- **transport**: the same level/event split the current jam uses.

Each effective boundary operation is recorded in that user's
`replay::scenario`. A gesture log is therefore ordered by logical steps, not by
wall-clock event timestamps.

### 5.2 Program and run identity

A program slot is:

```text
(user, program_revision, ir_hash, scenario/run, program instance)
```

A **run is tied to one recipe revision**. A recipe update starts a new
revision and a new run segment; it does not reinterpret the old journal through
a new graph. The old run remains replayable with its old IR. State continuity
for a live update is obtained explicitly by replaying the prefix into the new
program (see §6), and is recorded as continuity metadata on the new segment.

This is the same immutability rule the current jam uses for score branches:
history is never rewritten, and an edited future is a new identity.

### 5.3 Arrangement

An arrangement is an ordered set of program slots plus a master schedule:

```text
arrangement
  slot[user A]: recipe A + journal A + fresh program A
  slot[user B]: recipe B + journal B + fresh program B
  ...
  conductor:          one deterministic ui/audio block order
  generator/mixer:    consumes each slot's note/control outputs
```

The conductor advances the logical block schedule and steps every slot's
regions in the same documented order. This is the multi-program extension of
the current native conductor: one program becomes N, but the rule stays "apply
edits at a boundary, then tick".

Replay of an arrangement replays every slot's journal into a fresh program and
compares per-slot semantic outputs plus the mix. A per-slot digest is kept so a
failure localises to one peer. Branching an arrangement selects per-slot
recipe/input branches; the raw arrangement stays immutable.

## 6. Recipe revisions and live updates

A recipe update is an explicit sequence, never an audio-thread act:

1. Author and validate the new recipe IR; publish a new `program_revision` and
   content hash.
2. Lower it to a descriptor and construct a fresh program off the audio path.
3. Replay the current user's journal prefix into the new program, using the
   registered codecs. This reconstructs transport, score, voice, delay/scan
   state, crossings, and pooled values through the ordinary engine.
4. At a future musical boundary, swap the slot's program under the conductor.
5. Destroy the old program and its storage off the audio path.
6. Record the revision and continuity marker in the slot history.

The conductor never allocates, blocks, or rebuilds while ticking. The first
version may require the update to land at a loop/bar boundary; a later
checkpoint optimization (E3/I) can replace prefix replay.

## 7. Wire and multicast

The IR is semantic data; its wire encoding is versioned and owned by the tools,
consistent with the replay plan's R8.

- `clockr.program.v1`: `ProgramIR`, node/op/param/type/crossing records,
  content hash, format version.
- Jam-specific additions (additive, in a jam package): `ProgramRevision`,
  `InputJournal`/gesture entries, `ArrangementSnapshot`, branch requests.
- Core stays protobuf-free; the codec lives beside the debugger/tools.
- A server assigns per-user action sequence and revision numbers, validates
  the IR, and multicasts `(ir_hash, recipe_revision, journal entries)`.
- Every client validates and lowers the IR locally. Unknown ops or codecs are
  a reported fidelity limitation, not a silent skip.
- Recipes are content-addressed and immutable. A peer cannot mutate an old
  revision; it publishes a new one.

This is not CRDT merging. Each participant owns their graph and input log; the
arrangement is an ordered collection of authoritative per-user logs.

## 8. Validation and resource bounds

Validation happens before lowering and is mandatory on every client:

- format and op versions known;
- acyclic graph except through `delay` nodes (guarded recursion);
- node ids unique and in range; fan-in order preserved;
- ports, clock anchors, and crossing policies valid;
- every op's input/output types and params match its signature;
- every user param/seed has a registered codec, or fidelity is incomplete;
- bounded nodes, edges, fan-in, outputs, params, and encoded bytes;
- no allocation or unbounded work implied by a param.

A rejected program never runs. A program that throws at runtime needs an
arrangement policy (mute the slot, sticky error, or stop the arrangement); this
is the C3 error-policy decision applied per slot. One peer must not be able to
take down the mix.

## 9. Conformance and testing

- **Validation tests**: unknown op/version, bad type, cyclic graph without a
  delay, oversized graph/fan-in/params, missing codec.
- **Lowering tests**: node/edge/fan-in/clock/crossing parity with the
  hand-written recipe; owning and borrowing program constructions agree.
- **Sequencer parity**: per-step outputs and trace semantics equal to
  `jam::net::app` for a shared journal.
- **Replay parity**: IR program replays its journal twice and compares equal;
  a branch changes the suffix only.
- **Cross-platform**: native and wasm lower the same fixture to the same
  per-output digests; unsupported values report a limitation.
- **Update parity**: a revision built off-thread and prefix-replayed matches
  the old program at the swap boundary; no audio-thread allocation/rebuild.
- **Arrangement**: two slots with independent journals mix deterministically;
  a slot failure is isolated; branching one slot leaves the other untouched.
- **Multicast**: two clients receive the same IR/journal and produce identical
  digests; a stale revision is rejected or explicitly rebased.

## 10. Implementation phases

**P0 — decisions and skeleton.** Lock PIR1–PIR9; add the opt-in
`clockr::program_ir` module, IR types, canonical hash, and validation tests.

**P1 — primitive registry and lowering.** Core ops plus the pure primitives
needed by the sequencer (`daw.transport_bundle`, `daw.transport_step`,
`daw.playhead`, `jam.patch_apply`, `jam.schedule`, `jam.voice_tick`,
`jam.voice_mix`). Runtime descriptor builder; seeds materialised at
construction. No behaviour duplicated: op ticks call the existing pure
functions in `daw/transport.hpp` and `jam/*.hpp`.

**P2 — sequencer parity.** Emit the sequencer's canonical IR; prove output and
trace parity against `jam::net::app`; then switch the shell to the IR path.
Keep the hand-written recipe as a test oracle.

**P3 — per-user arrangement.** Program slots, per-user journals, master
conductor, generator/mixer, per-slot replay/compare. Two local slots first;
no network.

**P4 — recipe revisions.** New revision off-thread, prefix replay for state,
boundary swap, old run immutable.

**P5 — multicast.** IR + gesture journal over a validated gateway; native and
wasm clients; conformance fixtures. The existing jam server/gateway work is the
starting point.

**P6 — agent/MCP review.** Validated IR patches and gesture alternatives on a
branch, with human review and provenance, as in the existing plan.

## 11. Decisions to lock

| Id | Decision |
|---|---|
| PIR1 | Recipes travel as typed data, never as C++/wasm closures or raw descriptors. |
| PIR2 | The wire carries canonical expanded program IR; macros expand before transmission. |
| PIR3 | `lift`/`scan` name registered pure primitives with stable ids and versions; arbitrary lambdas are not shareable. |
| PIR4 | IR lowering produces a descriptor for the ordinary `kernel::program`; there is no second evaluator. |
| PIR5 | Params and seeds use replay-encoded values and host codecs; the debug schema is not a constructor. |
| PIR6 | A run is tied to one recipe revision; updates start a new run segment and preserve the old one. |
| PIR7 | Rebuilds happen off the audio path; state continuity uses prefix replay, then a boundary swap. |
| PIR8 | Validation and resource bounds are mandatory; unknown ops/versions/codecs are reported limitations. |
| PIR9 | The current step sequencer is the first parity target, not a special case. |

## 12. Open questions

- Exact op registry ownership: core built-ins vs application registration and
  how domain op versions are negotiated between peers.
- Whether the authoring layer is a C++ DSL, a JavaScript/editor AST, or both,
  and how macros are authored without introducing a second semantics.
- Gesture schema and sampling policy (per-block vs per-ui-tick; pressure,
  multi-touch, note gestures).
- The generator interface between a slot's note/control outputs and the
  arrangement mixer.
- Per-slot region set: does every program have ui/audio/worker, or only the
  regions its op graph uses?
- The first runtime arrangement API shape and how it composes with the existing
  native conductor.
- Error and resource policy when one slot fails while others keep playing.
- Wire encoding details and whether the canonical IR is also exported by the
  debugger for inspection.

## 13. What this preserves from the current jam

- The original run is immutable; edits create branches and new identities.
- Replay uses the ordinary engine into a fresh program.
- The lossy trace is never history.
- Static graphs and explicit crossings remain the rule; dynamic structure does
  not appear inside a tick.
- The current sequencer keeps working as the reference implementation and the
  first proof that the IR is sufficient.
