# Branchable live jam — agent-reviewed musical histories

*A proposed demonstration of clockr's replay, branching, and live-observation
ideas through a shared musical performance rather than a concurrency-bug demo.*

**Status:** partially implemented. The generic replay substrate
(`clockr::replay`), the jam's bounded musical model and fixed network, its
session/action log and deterministic metrics, a native conductor core, and a
first wasm shell/page are in the source tree. The operation is opt-in and the
DAW is its non-jam conformance consumer. Shared collaboration, the MCP tool
pack, debugger branch editing, and website staging remain open; see the
[implementation plan](branchable-live-jam-plan.md) for the current state.
The branchable jam is the **first-priority** demonstration. The concurrent
transport/event-interleaving workbench described below was also considered and
is retained as the **second-priority** companion demonstration. This remains a
design document, not an API specification.

**Intention.** The jam is one example application chosen to make clockr's
properties audible; it is not clockr's core proposition. clockr exists to make
concurrent coordination in audio software real-time-safe and predictable, and
the jam is a demanding consumer of that architecture rather than a redefinition
of it. §15 separates what the example genuinely exercises from what any
deterministic engine could provide.

**Implementation plan:** [`branchable-live-jam-plan.md`](branchable-live-jam-plan.md)
turns the phases below into file-level work for the core replay substrate and
the web jam application.

**Next direction:** [`branchable-live-jam-ir.md`](branchable-live-jam-ir.md)
proposes portable program IRs so each participant can own a static recipe and
input/gesture journal while every client runs every peer's graph. The current
fixed-network sequencer is its first conformance target.

**Companions:** `../reference/denotational-semantics.md` for the meaning of
clocks, epochs, state, and crossings; `frp-decisions.md` for the clocked
synchronous model and Shape C; `operator-sufficiency.md` for bounded dynamics
and the external-async boundary; and the roadmap section of the root
`README.md` for the currently open E1/E3/I and F1/F2 work.

---

## 1. The idea in one paragraph

Participants create music together by editing patterns, notes, and controls in
a shared visual environment. They do not write C++ and do not edit lowered
kernel nodes. An agent can attach through the debugging/MCP tooling, fork the
recorded history at a chosen point, propose a cleaned-up or otherwise changed
musical branch, and replay that branch for review. The original performance
remains immutable. At the end of a session, listeners can compare the raw shared
canvas with the agent-reviewed alternatives and hear their differences.

A useful short description is:

> **Branches and code review for a live musical performance.**

The agent is not a real-time performer and does not rewrite audio-thread state.
It proposes a validated patch against a recorded history. Humans decide whether
to accept, reject, or further edit that proposal.

## 2. Why this is a useful clockr demonstration

A concurrency demonstration based on a deliberately planted bug has a weak
answer ready for any skeptic: a careful conventional implementation could avoid
that bug too. This demo does not depend on that argument. Both the raw canvas
and the cleaned alternatives are legitimate artifacts.

The visible experience is:

- participants create a shared performance;
- the recorded history can be replayed;
- events and musical edits can be moved or changed on a branch;
- an agent proposes alternatives without destroying the original;
- raw and candidate performances can be auditioned and compared;
- the debugger reveals how the same clockr network produced each result.

The functional architecture explains *how* this is possible, but familiarity
with FRP is not a prerequisite for understanding the demonstration.

The clockr-specific claim is not "event sourcing makes replay possible". An
application can implement event sourcing by itself. The claim is:

> **Given a suitable boundary journal, clockr can reconstruct and re-execute a
> stateful, multi-clock program from a chosen prefix, using the same engine and
> semantics as the live performance.**

That includes transport and pattern state, `delay`/`scan` settling, region
epochs, `latest`/`queued` crossings, note scheduling, voice state, and the
actual debug trace. The jam makes that capability audible. Prior art and the
precise scope of the novelty are assessed in §14–§15.

### Second-priority companion: concurrent event-interleaving workbench

Before choosing the jam, we considered a debugger-centered demonstration of a
typically tricky concurrent DAW interaction. **This idea is still part of the
plan, but is explicitly second priority after the jam.** It is a companion
validation of the replay machinery, not a prerequisite for the musical demo.

The scenario combines transport control with background decoding:

- UI requests play, pause, seek, and loop changes while audio is playing;
- a worker decodes/prefetches audio and publishes progress, a usable watermark,
  completion, failure, or cancellation;
- the decoder can be stalled deliberately so that UI/audio/worker ordering
  becomes visible;
- a seek, pause/resume, decoder completion, and cancellation can all land near
  the same consumer epoch.

The debugger would let a user move **boundary events and region steps** to
explore different interleavings, replay the results into fresh programs, and
compare the original and candidate traces. Interesting edits include seeking
and pausing in the same UI epoch versus separate epochs, consuming two seeks
immediately versus after a backlog, resuming immediately before/after a decode
completion, and cancelling immediately before/after its terminal result.

The network under test should be **correct, not deliberately planted-buggy**.
Otherwise a skeptic can dismiss the demo as a situation that any sufficiently
careful implementation would already handle. The useful demonstration is that a
correct system exposes its legal interleavings, application properties, and
counterfactual behavior for inspection. Possible properties to check include:

- paused transport holds position, seek follows the documented priority rule,
  and resume continues from the intended landing;
- queued seeks retain producer-epoch order under the L6 capacity precondition,
  while `latest` controls may coalesce intermediate values;
- same-epoch setter permutations do not change behavior;
- the audio callback never waits for a decoder, and insufficient decoded data
  produces an explicit underrun rather than a hidden block;
- notes do not consume data beyond the published decode watermark;
- after cancellation has been observed, a stale load generation cannot become
  current;
- cross-region observations preserve their documented cut semantics, with any
  lapped fallback reported rather than presented as a perfect atomic cut.

The existing DAW P1 network already supports a smaller starting scenario: two
seeks while paused, then resume through a loop and a scheduled note. Interleave
UI publication with audio ticks in one run and create a UI backlog before the
first audio tick in another. This exercises FIFO seeks and latest-control
coalescing without implementing a decoder. The decoder stall/watermark part is
P2 work or a small explicit worker-completion fixture; it does not exist in the
current DAW P1 implementation.

This companion is more invasive than the jam's complete boundary journal.
Faithful arbitrary-live capture needs the effective input and final
crossing-observation records described in §6, including drain provenance,
queue outcomes, retries, and completeness. Unchanged exact-observation replay
and edited what-if simulation must remain visibly distinct. The decoder fixture
and worker observations must also be recorded as external stimuli rather than
re-executed as real I/O during replay.

The priority order is deliberate:

1. **Branchable jam first** — a direct, audible, collaborative experience whose
   value does not depend on convincing the audience that conventional code
   would contain a bug.
2. **Concurrent transport workbench second** — a deeper technical demonstration
   of arbitrary-live capture, event reordering, replay, schedule exploration,
   and application-level conformance properties.

Both demos must use the same generic scenario/run/branch and fresh-program
replay substrate. Neither may implement a second evaluator or redefine history
by rearranging recorded node-evaluation trace events.

## 3. Decisions already made in the design discussion

These choices constrain the first design unless a later decision explicitly
changes them:

1. **Musical patches change data inside a fixed clockr network.**
   Participants and agents do not change the recipe's structure for each edit.
   A later project may compile the musical representation into recipe structure
   to explore F2, but that is not the jam's initial mechanism.
2. **Replay is a generic clockr facility, not a jam-specific score replayer.**
   Another clockr application must be able to reuse the scenario recorder,
   fresh-program replay, and branch machinery without writing its own state
   rewind logic.
3. **Prefix replay comes before direct state snapshots.**
   A checkpoint initially means a scenario-log prefix. Seeking there constructs
   a fresh program and replays the prefix. Direct snapshots of ports, delay
   slots, links, epochs, and pooled values are a later optimization (E3/I).
4. **The general debugger replay design targets arbitrary live capture.**
   Native and wasm adapters should share the scenario/run model and conformance
   tests from the start. This larger track must not block the jam, whose
   complete boundary journal is a simpler input source.
5. **The original run is immutable.**
   Editing history creates a new scenario revision and run/branch identity; it
   never reorders old `TraceEvent` records or rewrites the raw performance.

A recommended, but not yet decided, deployment simplification is one
authoritative renderer at the venue with online participants editing through a
web interface. Synchronized audio on every remote participant's device is a
separate, substantially larger problem.

## 4. The responsibility boundary

The design must avoid two failure modes:

- **Too application-specific:** the jam reconstructs a score and schedules all
  notes itself, leaving clockr as a replaceable playback detail.
- **Too tightly coupled:** collaboration, musical taste, and agent policy leak
  into the kernel or generic debugger.

Use this boundary instead:

### Generic clockr/debugger responsibility

- Scenario, run, branch, and logical step identity.
- Boundary input and region-step recording.
- Any additional crossing-observation records needed for faithful live replay.
- Typed-value retention and materialization through registered host codecs.
- Fresh-program construction through a host-supplied factory.
- Prefix replay through the ordinary `engine::step` path.
- Branch execution and original/branch trace comparison.
- Debugger commands for inspecting, editing, and replaying scenarios.
- Explicit completeness/fidelity information: a lossy or partial journal must
  not be presented as exact replay.

### Jam application responsibility

- `Score`, `Pattern`, `ScorePatch`, track, instrument, and voice definitions.
- Pure functions that interpret those values and arrange musical events in
  time.
- A fixed, bounded track/voice network.
- The visual musical editor and musical diff view.
- Collaboration, participant identity, permissions, persistence, and revisions.
- Agent analysis, proposed patches, provenance, and human review.
- Policies for when a reviewed candidate may affect the live performance.

The litmus test is:

> Could another clockr application replay and branch its own program using the
> same machinery, without implementing a second evaluator or application-
> specific state restoration?

If not, too much of the value has moved into the jam.

## 5. Musical patches live in the network

The existing DAW construction, `daw::net::app(score)`, captures its score in
scheduler closures. That is appropriate for the fixed DAWmulator score but not
for live editing. The jam should reuse the pure transport/scheduling ideas while
moving the editable score/pattern into explicit network inputs and state.

Conceptually:

```text
ui region
  participant/agent ScorePatch inputs
           │
           │ explicit crossing policy
           ▼
audio region
  pattern state ──▶ pattern + transport evaluation ──▶ note/control events
       ▲                    │
       │                    ▼
       └──── scan state     voice/gain/output state
```

The important properties are:

- score updates are typed values at clockr boundaries;
- the current score/pattern is state-as-data, not a mutable closure accessed by
  many callbacks;
- pure functions evaluate musical events over the current transport interval;
- existing engine evaluation produces all raw and candidate note streams;
- new pattern versions are constructed off the audio thread and have explicit
  ownership through the normal user-value machinery;
- tracks and voices have fixed bounds and an explicit overflow/voice-stealing
  policy.

The pattern representation can support operations such as sequence, stack,
transpose, reverse, rotate, quantize, mask, and density reduction as data or as
pure functions over data. It is not necessary to create one kernel node per
note or per musical transformation for the patch to be part of the network.
`LoopRegion`, `TransportIn`, and `PosPair` already illustrate the distinction
between cohesive typed data and graph structure.

A natural first transport for independent participant edits is `queued`, since
`latest` intentionally coalesces intermediate values and could discard distinct
edits. Whole-canvas snapshots may use `latest` when only the newest revision is
meant to matter. The final choice needs capacity, backpressure, and revision
semantics; it is not merely an implementation detail.

Within one epoch, inputs form a map. Permuting writes to distinct ports must not
change the result (L9a). Two patches that require an order must be combined into
one value or assigned to distinct epochs; the editor must not invent an
intra-epoch event order.

## 6. Scenario replay and branching

### Replay uses the real engine

For an unedited replay:

1. Construct a fresh `kernel::program` from the same recipe/descriptor and
   construction options.
2. Use the same initialization, including the initial UI publication.
3. Apply the recorded boundary operations and region steps.
4. Evaluate with the normal engine, not a JavaScript or application-side copy
   of the scheduling algorithm.
5. Compare semantic values by step/node/output, ignoring timestamps, measured
   durations, allocation addresses, and `user_cell*` identity.

Delay state and cross-region transport state are recreated by executing the
prefix. This is why prefix replay is simpler and safer than initially copying
only `delay_states` or output values.

For an edited branch:

1. Copy the scenario and record its parent/revision.
2. Change boundary events or their logical step placement.
3. Replay the unchanged prefix.
4. Execute the edited suffix into a fresh run/branch history.
5. Display original and candidate traces side by side without modifying the
   original.

Node-evaluation order inside a region remains topological. Reordering debugger
`TraceEvent` objects would falsify history and is not a replay operation.

### Identity cannot be only `(clock, epoch)`

A throwing tick publishes no epoch, so a retry can reuse the attempted epoch.
Scenario and trace alignment therefore needs stable run and logical step IDs.
Epochs remain useful display and resampling metadata, but are not sufficient
identity for time travel or original/branch comparison.

Wall-clock timestamps are also not semantic IDs. Native recorder drain order is
not a complete cross-thread execution order, and native/wasm timestamps cannot
provide a common ordering authority.

### Two journal sources, one replay facility

The generic design should accommodate:

1. **Explicit boundary journals.** These contain effective input operations and
   an explicit region-step schedule from program initialization. Scripted
   scenarios, wasm, and the jam use this complete-by-construction source.
2. **Arbitrary live observation journals.** Faithful native replay additionally
   needs effective pre-step inputs, final drain frames and provenance,
   publish/drop outcomes, retries/lapped-fallback information, and completeness
   accounting. A current trace alone is not enough.

A useful implementation distinction for the second source is:

- **exact-observation replay**, which reproduces recorded inbound observations;
- **what-if simulation**, which switches at a declared branch boundary to a
  deterministic transport/schedule and is labelled as a semantic alternative,
  not as the uniquely correct future of the original OS threads.

The jam does not depend on solving the arbitrary-live capture problem first.
Both sources should feed the same scenario/run/branch and fresh-program replay
architecture.

### No late-start exactness and no silent loss

Starting an ordinary recorder in the middle of a stateful run does not provide
a replayable prefix. Exact replay requires either an always-on prefix journal
from a known initialization or a real checkpoint. Recorder overflow, disabled
capture, history eviction, missing values, or unsupported codecs must mark the
journal incomplete and prevent an "exact" claim.

The existing bounded live trace recorder is allowed to drop events. It must not
silently double as a lossless replay journal.

## 7. Two timelines: edit history and musical time

The jam must keep these axes distinct:

- **Edit history:** participant and agent operations, scenario revisions,
  branch points, and review decisions.
- **Musical time:** beats, bars, loops, note start/end positions, and transport
  epochs.

A branch is created at an action-log prefix, while its edits normally target
musical positions. For example:

> Fork after boundary operation 417 and reduce drum density in bars 8–12.

A bar alone cannot identify a scenario branch point when participants edit
while the transport loops. The UI should map the two timelines without
collapsing them into one.

Likewise, accepting a candidate does not rewrite the audible past. For live
adoption, record the acceptance as a new action and apply the accepted score at
a future beat/bar/loop boundary. For an end-of-session comparison, render the
raw and candidate histories independently; no live topology or state swap is
needed.

## 8. Agent and MCP workflow

A safe agent review flow is:

1. Select the raw scenario and branch prefix.
2. Create an agent-owned branch tied to an exact base revision.
3. Read the high-level musical representation and deterministic metrics.
4. Propose a structured patch rather than a replacement trace.
5. Validate schema, base revision, musical bounds, track/voice capacities, and
   user-specified preservation constraints.
6. Replay raw and candidate histories into fresh programs.
7. Present the musical diff, actual note-event/output diff, and A/B audition.
8. Let humans accept, reject, or edit the proposal. Record that decision in the
   history.

Possible deterministic metrics include event density, simultaneity, register
spread, duplicate notes, unresolved sustained notes, clipping risk, and voice
pressure. These can assist analysis without pretending that musical quality is
objective. "Coherence" is a review criterion chosen by participants, not a
license for autonomous replacement.

The existing `tools/debug-mcp` path is the natural integration point after
scenario/branch operations stabilize. Musical operations may be exposed as
application-specific MCP resources/tools beside generic debugger operations.
The agent runs off the real-time path; program construction, candidate replay,
model work, and review never occur inside an audio tick.

## 9. Collaboration, playback, and security

For the first shared deployment, prefer:

- one authoritative collaboration/session service;
- server-assigned action sequence and revision numbers;
- optimistic concurrency for human and agent patches;
- one authoritative audio renderer;
- web participants editing high-level musical data;
- local/server-side privileged debugger and MCP access.

A centralized ordered log avoids requiring CRDTs or decentralized merge logic
in the first implementation. A stale agent patch should be rebased deliberately
or rejected, never applied silently over unrelated participant edits.

The existing debugger is a loopback debugging interface with powerful
write-back operations, not an authenticated internet service. **Do not expose
it directly to online participants.** Put an application-specific, validated
gateway in front of collaboration. Keep debugger injection and MCP control on a
privileged local/server-side boundary.

Synchronized audio for every remote participant is a later feature. It requires
choosing between local wasm rendering with clock synchronization/drift handling
and streamed authoritative audio, with the associated latency and
infrastructure. A single-web-worker wasm run also cannot demonstrate native
thread safety; native and wasm should use shared scenario conformance tests but
make different fidelity claims where necessary.

## 10. Suggested implementation sequence

### Phase A — Generic replay primitives

- Define scenario/run/branch/logical-step identity and revisions.
- Add a host replay factory and typed-value codec/materialization capability;
  the current debug schema is descriptive and is not a general runtime
  constructor for user structs.
- Implement complete boundary journaling and fresh-program prefix replay.
- Add immutable scenario branching and semantic output/trace comparison.
- Develop native and wasm adapters against the same document/protocol model
  and portability tests.

### Phase B — Fixed musical network

- Define the bounded pattern/score/patch representation and its pure semantics.
- Move editable musical data out of captured score constants and into explicit
  input/state.
- Reuse the DAW transport/scheduling functions where appropriate without making
  the canonical fixed-score DAWmulator less clear.
- Add golden tests for notes, offsets, voice state, patch application, capacity
  behavior, and debug/release parity.

### Phase C — Single-host visual branching

- Build a small high-level note/pattern editor; do not turn lowered kernel nodes
  into the live-coding language.
- Record participant edits as generic boundary scenario operations.
- Support manual fork, event move/replacement, replay, and A/B listening.
- Show original and candidate runs in the debugger with changed values/events
  highlighted.

This phase already demonstrates the central idea without an agent or network
collaboration.

### Phase D — Agent-reviewed alternatives

- Add agent-owned branches and the musical patch format.
- Expose the required scenario/branch operations through MCP.
- Add validation, metrics, candidate replay, provenance, and human review.
- Ensure shutdown/cancellation and candidate destruction occur off the audio
  thread.

### Phase E — Shared online canvas

- Add the authoritative session gateway, participants, permissions, ordered
  operations, revision conflicts, and persistence.
- Start with one renderer and end-of-session raw/candidate comparisons.
- Rehearse the actual demo with conflicting edits, stale agent proposals,
  disconnects, and a long session history.

### Later, independently useful extensions

- Live candidate adoption at future musical boundaries.
- Periodic direct checkpoints to accelerate long prefix replay (E3/I).
- Arbitrary-live faithful native replay and more advanced schedule exploration.
- Remote synchronized listening.
- Selective acceptance or merging of individual agent patch operations.
- A musical-IR-to-recipe compiler as a separate higher-order/F2 demonstration.

## 11. Acceptance criteria for the demonstration

The smallest credible demo should establish all of the following:

- The raw performance and every candidate are produced by the actual clockr
  engine from the same fixed recipe, not by separate musical schedulers.
- Replaying an unchanged supported scenario reproduces semantic outputs at each
  step; timings, addresses, and trace transport order are excluded deliberately.
- Editing or moving a score-patch boundary event changes outputs through normal
  graph evaluation.
- Same-epoch setter permutations preserve behavior, while moving events across
  epochs exposes the declared crossing semantics.
- Original history remains unchanged while branches are created, replayed, and
  destroyed.
- Native and wasm adapters agree for their explicitly supported values and
  scenarios, or report a clear fidelity limitation.
- The agent cannot mutate the raw run, bypass patch validation, or apply a stale
  proposal without review.
- Agent work, candidate construction/replay, and review do not allocate, block,
  or rebuild programs on the audio thread.
- Incomplete journals and unsupported values are visible and cannot be labelled
  exact replay.
- A listener can hear and inspect the difference between the raw shared canvas
  and at least one reviewed alternative.

## 12. Explicit non-goals for the first version

- Structural live coding of `shell::recipe` graphs.
- F2 topology replacement for every note or pattern edit.
- Autonomous acceptance of an agent's subjective musical judgment.
- Rewriting already-audible history.
- CRDT/decentralized collaboration.
- Synchronized remote audio on every participant's device.
- Exact replay from a late-started or lossy trace.
- Generic replay of arbitrary user structs from today's descriptive protobuf
  schema without host-registered materialization support.
- Claiming that a virtual post-branch schedule predicts the original threads'
  exact future.

## 13. Existing code to reuse and inspect

- `demo/daw/daw/network.hpp` — transport, queued seek, latest controls,
  playhead feedback, scheduling, and voice state. Its captured score identifies
  what must become explicit input/state for the jam.
- `demo/daw/daw/transport.hpp` — pure musical functions and existing wire
  structs to reuse where they fit.
- `demo/daw/daw/dawmulator.hpp` — canonical recipe and initialization ordering.
- `demo/daw/tests/` — examples of musical golden tests and parity checks.
- `tools/web-debugger/` — live graph, trace recorder/history, timeline,
  injection seam, protocol, and browser UI.
- `tools/debug-client/` and `tools/debug-mcp/` — existing agent-facing client
  and MCP layers; connect-time "history replay" there currently resends traces,
  it does not re-execute a program.
- `demo/daw/shell/` — wasm construction and serialized worker stepping, useful
  for a second replay adapter but not evidence of native thread behavior.
- `../reference/denotational-semantics.md` — L1/L2/L5/L7/L9 and the state-as-data
  assumptions on which replay depends.

## 14. Related work and prior art

The jam sits at the intersection of four bodies of work. None of them covers
the whole combination; several cover one axis well enough that the jam should
position itself against them explicitly. §15 then asks which parts of the
novelty are actually enabled by clockr.

### Versioning and branching in live and creative coding

- **SHARP** (Manesh, Bowman Jr., Lee, *Creativity and Cognition* 2024,
  <https://doi.org/10.1145/3635636.3656195>; work-in-progress at NIME 2023,
  *Supporting Exploration and Rapid State Navigation in Live Coding Music*) is
  the closest system. A Tidal code-editor extension builds a **per-instrument
  version tree**: each run appends a state-history node, selecting any node and
  re-running the block **creates an explicit branch**, and nodes can be
  tagged, previewed, and recalled during a performance. It shows that live
  coders want and can use branches, but keeps history per code block and does
  not construct a fresh program, replay a boundary journal, or propose and
  review patches.
- **CodeBank** (Kirkbride, 2019, <https://doi.org/10.5281/zenodo.3946183>)
  brings a Git/Mercurial-shaped workflow to collaborative FoxDot: performers
  pull codelets from a central repository, experiment locally, and push a
  snippet to the shared server. This is the nearest VCS-inspired collaboration,
  without branch semantics over recorded time.
- **Variolite** (Kery, Horvath, Myers, CHI 2017,
  <https://doi.org/10.1145/3025453.3025626>) introduced graph-structured
  "informal version control" for exploratory code; SHARP cites it as the
  ancestor of its version tree.
- **Quickpose** (Rawn et al., CHI 2023,
  <https://doi.org/10.1145/3544548.3581394>) studies version control for
  creative coding as material interaction. **VRGit** (Zhang et al., CHI 2023,
  <https://doi.org/10.1145/3544548.3581136>) visualises versions and branches
  as a graph with synchronous awareness. **"Towards Creative Version
  Control"** (Sterman, Nicholas, Paulos, CSCW 2022,
  <https://doi.org/10.1145/3555756>) and the CAD-branching studies (Cheng et
  al. 2023, <https://doi.org/10.1145/3610220>; Deng et al. 2026,
  <https://doi.org/10.1145/3772318.3791061>) document how creative
  practitioners actually use histories and branches.
- **"Probing the Design Space: Parallel Versions for Exploratory
  Programming"** (Beckmann et al. 2025,
  <https://doi.org/10.22152/programming-journal.org/2025/10/5>) is the
  programming-language-systems counterpart: parallel versions as a first-class
  exploratory construct.

### Time as a queryable quantity in live music

- **TidalCycles** and its JavaScript port **Strudel** treat a pattern as a pure
  function of time, so any interval can be queried and re-evaluated
  arbitrarily. That referential transparency is the semantic precondition for
  deterministic replay, and it is why a branch can be re-evaluated rather than
  re-animated from a recorded event order. Alex McLean's thesis
  (*Artist-Programmers and Programming Languages for the Arts*, 2011) is the
  theoretical source.
- **Sonic Pi** exposes **Time State** (`set`/`get` with a `delta`) for reading
  past events; its v2 timing model was given a formal temporal semantics by
  Aaron, Orchard, and Blackwell (2014,
  <https://doi.org/10.1145/2633638.2633648>).
- **Gibber** was extended to networked ensembles with shared editing, remote
  execution, and clock synchronisation (Roberts et al. 2015,
  <https://doi.org/10.5281/zenodo.19351>); **Lee and Essl** (2014,
  <https://doi.org/10.5281/zenodo.1178846>) address shared namespaces, state
  conflicts, and distributed execution. **Extramuros**, **Estuary**, **Flok**,
  **Vivace**, and **Synerg(e)ia** are collaborative live-coding platforms with
  shared sessions. These solve multi-user coordination, not immutable
  branch-and-replay history.
- **"Live Coding: A User's Manual"** (Blackwell, Cocker, Cox, McLean,
  Magnusson, MIT Press 2022,
  <https://doi.org/10.7551/mitpress/13770.001.0001>) and **"Live Coding: A
  Review of the Literature"** (2021,
  <https://doi.org/10.1145/3430665.3456382>) are the field's general
  references.

### Replay, time travel, and executable history

- **Perez and Nilsson, "Testing and debugging functional reactive
  programming"** (ICFP 2017, <https://doi.org/10.1145/3110246>) is the closest
  semantic analogue: referential transparency lets them record and replay
  runs, scrub along the execution timeline, and check temporal-logic
  assertions. It validates the *principle* the jam auditions — replay through
  the real evaluator — on FRP games.
- **"SpaceTime Programming: Live and Omniscient Exploration of Code and
  Execution"** (Döderlein et al., 2026,
  <https://arxiv.org/abs/2603.18735>) unifies code variants ("space") and
  execution history ("time") in one trace, with a live omniscient debugger.
  It is the nearest general-programming formulation of "branch and
  re-execute".
- **Multiverse debugging** (Torres Lopez et al., ECOOP 2019,
  <https://doi.org/10.4230/LIPIcs.ECOOP.2019.27>) observes all legal
  interleavings of a nondeterministic program — the direct analogue of the
  second-priority event-interleaving workbench.
- Omniscient and record/replay debugging — Bil Lewis's "Debugging Backwards
  in Time", **rr**, WinDbg **TTD**, **TARDIS**, **McFly** — establish the
  general mechanisms (deterministic recording, prefix replay, state
  restoration) that §6 adapts to a ticking FRP program.
- **Event Sourcing** (Fowler, 2005) is the architectural pattern behind
  scenario/run/branch identity: append-only facts, rebuild by replay, never
  rewrite the past.

### Branchable data and asset tooling

- **Dolt** ("Git for data"), **Noms**, **Irmin**, and **Pijul** provide
  branches, merges, and historical queries over versioned data. **Figma
  branching**, **Plastic SCM**/**Perforce** for game assets, and
  Scratch/OpenProcessing "remix" show the same fork model in creative tools.
- DAWs offer history but not branch semantics: Ableton **Capture MIDI** and
  **take lanes**, Bitwig **retrospective recording**, Ardour **session
  snapshots** and undo history, Splice project versioning. They snapshot or
  undo; they do not fork an immutable, replayable performance.

### What this means for the claim

We find no system that combines collaborative live-coded music, immutable
event-sourced runs, fork/replay into a fresh evaluator, and agent-proposed
patches under human review. **SHARP is the nearest neighbour**, and §2's claim
should be read alongside it: SHARP shows the musical interaction is real; the
jam's contribution is to make the branch a *re-execution of the same program*
under clockr's clock, crossing, and state semantics, rather than a recall of
saved code states.

## 15. Is the novelty enabled by clockr, or accidental to the jam?

A useful test: an architectural property is **enabled** if it follows from an
invariant clockr already commits to and a plausible alternative implementation
would violate it; it is **accidental** if it could be bolted onto any
deterministic engine and the demo would be just as convincing without the
architecture. Applying that test splits the claim in two.

### What clockr genuinely contributes

The replay/branch core is enabled, not accidental, and the mapping is
specific:

- **Programs are descriptions plus a restartable interpreter.** `program`
  either owns a recipe or borrows a descriptor, and `engine.step(program,
  clock)` carries no ambient state. "Construct a fresh program and replay the
  prefix" is therefore the *normal* execution path, not a second evaluator.
  §11's "produced by the actual clockr engine" criterion follows directly.
- **State is data inside the program.** `delay`/`scan` state, region epochs,
  and pooled values live in the program and are recreated by executing the
  prefix (§6). A trace-replay or score-replay design cannot reproduce that
  settling; this is what makes replay faithful rather than cosmetic.
- **Clocks are anchored at boundaries and crossings are explicit.** The
  boundary journal is well-defined because inputs enter at `input`/`output`
  anchors and cross regions under declared `latest`/`queued` policies. Without
  that discipline there is no complete record of external influence to
  replay.
- **Determinism and epoch atomicity give replay something to compare.**
  L1/L2 make evaluation order unobservable; L9 makes a multi-clock program a
  function of per-epoch input maps plus a fixed inter-epoch order, which is
  exactly what "replay the prefix, then fork" needs. Same-epoch permutation
  invariance (L9a) is what lets the editor avoid inventing an intra-epoch
  ordering.
- **The trace is not the source of truth.** Reordering `TraceEvent`s is
  forbidden because the trace is an observation stream; replay re-executes.
  This stance is what distinguishes the jam from log-replay demos.
- **Real-time safety is what lets live and replay coexist.** Construction,
  candidate replay, and model work stay off the audio thread, and evaluation
  does not allocate. A replay demo in a non-real-time system would not need
  this, and would not be as strong a demonstration.
- **Parity (L5) makes the guarantee auditable.** The debug and release engines
  must agree, so a traced run and its replay can be compared without wondering
  which engine produced which.

### What is application-level, not architecture-enabled

- **Collaboration** (ordered log, revision numbers, optimistic concurrency,
  the validated gateway) is generic distributed-systems design. The choice of
  a central log over CRDTs is a scoping decision, not a clockr consequence.
- **Musical representation and patch semantics** are the application's. Moving
  score/pattern out of captured closures into network input/state (§3, §5) is
  *required by the jam*, not by the kernel; the current DAW's captured score is
  a counterexample inside clockr itself.
- **Agent/MCP review, provenance, and policy** are application plus tooling.
  Nothing in the kernel constrains how a proposal is validated or accepted.
- **Branch identity and immutability** are a scenario-layer design. They use
  `engine::step` but could sit on any deterministic stepping engine.
- **A/B audition and the two-timeline UI** are purely presentation.

### The honest qualification

The novelty is **compositional, not categorical**. SHARP already branches
live-coded music, Dolt already branches data, and Perez and Nilsson already
record and replay FRP. clockr does not make branching *possible* in a way no
other system could; it makes a particular guarantee cheap and honest: the
branch is the same program, re-executed under the same clock, crossing, and
state semantics, alongside the immutable original. That is a sufficiency
claim, and §2 states it as one.

Two things would make the novelty accidental, and the design already avoids
them: a replay that needs a second evaluator or a JavaScript re-implementation
of the scheduler (§6 forbids this), and a history defined by rearranging trace
events (also forbidden). One thing would weaken it, and the design must resist
it: letting the descriptive debug schema stand in for runtime reconstruction.
Until host codecs and a replay factory exist (§10 Phase A), the enabling
properties are latent in the architecture rather than demonstrated.

**Verdict.** The replay-and-branch core is legitimately enabled by clockr's
existing invariants — fresh-program construction, state-as-data,
boundary-anchored clocks, explicit crossings, determinism and epoch atomicity,
and real-time safety — and is not an accident of the jam. The collaboration,
musical-patch, agent-review, and UI layers are application design that would
work on other engines. The defensible claim is therefore narrower than
"branching live music needs clockr": it is that clockr can make a branch a
faithful re-execution of the same stateful, multi-clock program, without a
second evaluator or a rewritten history — with SHARP as the baseline showing
the musical interaction is real.
