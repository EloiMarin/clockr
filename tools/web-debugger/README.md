# clockr debugger

A live, browser-based debugger for an SF network: it renders the network as a
graph of combinator-labelled nodes and lets you scrub the evaluation trace
tick by tick, highlighting which node is evaluating and showing the value it
produced.

The **server** is native-only (it needs threads and sockets, so it is not part
of the wasm build) and is gated behind the `CLOCKR_BUILD_DEBUGGER` CMake
option (default `ON`), because it fetches protobuf and uWebSockets. On Windows
uWebSockets also needs libuv (uSockets has no epoll/kqueue there), so that
configure fetches libuv as well.

The **web UI** also has an in-browser transport: `?local=1` makes it listen on
the same-origin `clockr.debug.local` BroadcastChannel instead of `/ws`. The
wasm DAWmulator's worker feeds that channel with JavaScript graph/event
objects, so the hosted demo can show the debugger against a program that never
leaves the browser — no server, sockets or protobuf in that path. The native
server remains the transport for live processes and the MCP client.

```
./build.sh
./_build/<preset>/tools/web-debugger/debugger_demo
# open http://localhost:9002
```

A live deployment can run `dawmulator_native` instead: its embedded debug
server serves this same UI on 9002, so the public debugger can show the live
DAW graph and trace. `debugger_demo` remains the local synthetic showcase.

## Examples

The demo host registers one example, **`demo`** — the synthetic showcase: all
three regions, a `latest` and a `queued` crossing, `hold`/`delay` state, a
described user struct crossing a clock (rendered inline with its fields), and
a lift that throws every `--throw-every N` ui ticks (red ticks to browse).

The server itself supports multiple examples, switched at runtime from the
toolbar's example selector; switching is a request, not local state: the server flips the live index,
quarantines every recorder's backlog (a thread mid-step across the flip lands
in the old recorder, which is never drained while another example runs),
clears its history, and broadcasts the new `GraphDescription` — and every
client, including the one that asked, resets its whole view on any graph
message. Re-selecting the live example (or an unknown name) is a server-side
no-op, so the view never flickers. Capture stays exactly as paused/resumed:
switching while paused shows the new graph with an empty timeline until you
resume.

## What you see

- **Graph** — one box per lowered node, labelled with the combinator that
  emitted it (`lift`, `hold`, `delay`, …). Boxes are grouped into lanes by
  thread (`audio`, `ui`, `worker`, top to bottom); cross-thread edges are
  dashed and tagged with their resampling mode (`latest` / `queued`). Input
  and output ports render as small tabs centered on their lane's left and
  right edges — inputs left, outputs right, each hanging half outside the
  region like a patch-bay jack and sitting near its connected node's
  height — so a lane reads as a region with a boundary aligned to the
  signal flow. Edges always leave from the right edge and enter
  through the left, so data visibly flows left to right; the rare backwards
  edge (delay feedback) loops around the right and comes back in on the left.
  ELK (vendored, see `web/vendor/`) orders the
  DAG into columns and the page snaps the result into measured clock bands,
  so boxes never overlap and never leave their lane. Drag the background to
  pan, wheel or pinch to zoom around the pointer, use the floating `−`/`+`/`⤢`
  controls to zoom or fit, and drag a node to reposition it (nodes stay
  clamped to their clock band, port tabs slide along their rail, and edges
  follow both). Nodes carry accessible names and titles, so the graph is usable
  with a screen reader.
- **Timeline** — a wall-clock scrubber, one lane per thread, with faint
  gridlines and a seconds legend like an audio editor. Each region tick is a
  vertical line on its lane (thread-colored; red when it threw). The present
  is the vertical line pinned to the visual center; the inspected tick is
  always the latest tick at or before it. Drag or two-finger swipe to pan,
  wheel or pinch to zoom (there are `+`/`−` buttons too) — the cursor never
  leaves the recorded span, and you cannot zoom out past it. Tapping a tick
  line jumps the present exactly onto it.
- **Cursor** — within the inspected tick, step node by node, one row per
  evaluation (arrow keys, or the transport buttons). The node under inspection
  is highlighted light green, a node that threw red, and each completed node shows its
  output value in a small chip on its output edge — port tabs show it inline
  instead, so the gutters stay clear. Each row also carries the
  node's measured evaluation time (`12.3 µs`) — quiet by design, interesting
  only when something is slow — and the cursor line reports the inspected
  tick's total time the same way. The event list on the right mirrors the trace
  for the selected tick. The tick buttons move the
  present onto the neighboring tick in time; the region buttons jump within
  the inspected tick's own lane, skipping every other thread's ticks.
- **User structs** — a value of a described user type renders inline as
  `type{field=value, …}`, using the per-type schema the producer ships with
  the graph description (nested structs recurse). A `std::string` member
  renders quoted (a `text` field); an `enum` member renders its enumerator
  name (`enum_field`), falling back to the underlying integer for a value not
  in its table. An `optional` member renders its payload when engaged, absent
  (`∅`) when disengaged. A type with no schema — an anonymous struct, or one
  this client does not know — renders as `struct`.
- **Live** — follows the newest tick and captures. Leaving live mode via any
  gesture, button, or the checkbox pauses capture as well, so the ticks you
  are inspecting cannot be evicted from the history while you look at them;
  re-check live to resume. Space toggles live, `←`/`→` step events,
  `shift+←`/`shift+→` step ticks, `+`/`−` zoom. If the connection drops while
  live, the status reports it and the page retries; while paused it stays
  quiet and reconnects when you go live again.

## How it fits together

```
ticking threads (one debug_engine each)          server thread (uWS loop)
  debug_engine ──▶ trace_recorder ──▶ spsc queues ──▶ drain every 33ms
        events with (thread, epoch)                   ├─ history tail (replay)
                                                      └─ EventBatch ▶ WebSocket ──▶ web UI
                                                             GraphDescription on connect
```

- **`engine` region hooks.** `kernel::tick` brackets each region tick with
  `on_region_step_begin` / `on_region_step_end` and stamps the region's
  `published_epoch`; `debug_engine` turns those plus the per-node steps into
  a `trace_event` stream. A throwing node emits `node_tick_throw` and the
  step's `region_step_end` reports `threw = true`. See
  `src/kernel/README.md`.
- **`trace_recorder`** (`server/recorder.hpp`) is a `trace_sink` that hands
  events off through one bounded lock-free `conc::spsc` per thread tag,
  stamping each with the producer clock. A full queue drops the event and
  counts it — an audio thread never blocks on the debugger. Capture can be
  gated on/off from the client (`pause` / `resume`). A traced value that is a
  user struct is reference-counted: the recorder retains it when it queues the
  event and the server releases it once encoded, so a struct's cell cannot be
  recycled while its trace is being rendered.
- **`server`** (`server/server.hpp`) serves the static page and the `/ws`
  endpoint. It binds `server_options.host` (default `127.0.0.1`) with an exclusive port.
  On connect it sends an `ExampleList` (the live examples and which is active),
  the active example's `GraphDescription`, replays the history tail, then
  broadcasts batches drained on a fixed cadence (30 Hz by default).
  A `select_example` client message flips the live example and broadcasts the
  new graph to every client. The example set is dynamic: an embedded host
  registers one example per running instance with `add_example` and drops it
  with `remove_example` (both callable from any thread; removal is synchronous
  with the loop, so the host may destroy the recorder once it returns). The
  loop broadcasts a fresh `ExampleList` on any change, and the page rebuilds
  its selector from it. Every server object is independent,
  so several may coexist as long as their ports differ.
- **Lifecycle.** `run()` blocks until `stop()`, SIGINT, or a listen failure.
  `stop()` is safe from any thread and any number of times: it flags the
  shutdown and wakes the loop, and the loop thread then closes every client
  connection, the drain timer and the listen socket, so `run()` returns and
  the thread joins. An embedded host (a plugin serving its live program)
  constructs one `server` around its program and recorder, steps through one
  `debug_engine` per ticking thread, runs `run()` on its own thread, and calls
  `stop()` on unload — no thread or socket may outlive the module.
- **History is client-side.** The page records every event it receives and
  caps the history (see `MAX_TICKS` in `web/app.js`); the server's replay tail
  is only there so a reconnect or a late client is not empty.
- **Graph layout** (`web/layout.js`) is a pure module so it can be tested
  under node. It runs ELK once for ordering (left-to-right layering) and then
  imposes the lane geometry itself: each clock becomes a horizontal band whose
  height is measured from the rows it holds, nodes keep ELK's column and are
  stacked into rows, and edges are routed as orthogonal polylines that always
  exit right and enter left (backwards edges loop around the right side and
  come back). Fan-in shares the target's edge instead of merging: with *n*
  inputs the wires attach at 1/(n+1) … n/(n+1) of its height. ELK's own
  layout partitions were tried first and rejected — see `GOTCHAS.md`.

## The schema

`proto/clockr_debug.proto` (package `clockr.debug.v1`) is the wire
contract. It is deliberately versioned by package name so other clients
(MCP/skills) can pin it; the `Value` oneof mirrors the kernel value set
(scalars plus `struct_value`, and `string_value` for `text`/enum fields) and
grows an alternative rather than renumbering.

User structs travel as `StructValue{type_id, fields}` — one `Value` per
described field, positionally matched to the `TypeSchema` with that id, which
is sent once in `GraphDescription.types`. The producer walks the type's field
table (name, offset, kind, width) as pure data; no user callback runs on the
server thread. A type without a `describe` specialization has id 0 and no
schema, and renders as a placeholder.

- **C++**: `protoc` is built from the fetched protobuf and run by a
  `add_custom_command` to generate `clockr_debug.pb.{h,cc}` in the build
  tree; `server/message.hpp` and its `.cpp` implement the conversions.
- **JavaScript**: the page has no protobuf runtime. `web/protobuf.js` is a
  minimal proto3 wire reader and `web/schema.js` maps the field numbers to
  names; `web/schema_smoke_test.js` (run by `ctest`) pins the mapping. When
  the schema changes, update both files and the test.
- **One message per evaluation**: the wire used to carry each node evaluation
  as an adjacent `NODE_EVAL_BEGIN` / `NODE_EVAL_END` pair that the page
  folded. The split never earned its keep, so the pair collapsed at the
  protocol level: the end/throw message now carries the value, the measured
  time, and the outcome, and producers no longer emit the begin. (The enum
  keeps `NODE_EVAL_BEGIN` for the engine hook; see the note in the `.proto`.)


## Write-back commands (the agent path)

The observation stream shows what the program does; the write-back path lets a
client *change* it and read values back — the interactive feedback loop an
agent needs to debug a live host. This tree implements the protocol and the
server/host side; the TypeScript client and the MCP server are the sibling
packages `tools/debug-client` and `tools/debug-mcp`.

The one real problem is thread ownership. `program::set`/`get` are
same-thread-with-the-owning-region contracts, so the server's uWebSockets loop
thread must never touch a ticking program. The server therefore only
*enqueues*; the ticking thread applies at the boundary, through the engine's
`step_hook` — the debugger's `debug::command_hook`:

```
agent ──SetInput / ReadInput / ReadOutput──▶ server (uWS loop thread)
                                               │ bounded SPSC, one per (example, region)
host ticker ──────────────────────────────────▶│ engine.step(program, clock)  ← unchanged
                                               │   └─ step_hook::before_step:
                                               │        program.set / snapshot reads
                                               │   └─ step_hook::after_step:
                                               │        Ack stamped with the consumed epoch
agent ◀── Ack / PortValue (that connection) ───┘   (+ CommandStats broadcast on drops)
```

Applying at a tick boundary is the semantically correct atomicity, not a
workaround: in a clocked-synchronous program a value set between ticks is
indistinguishable from one set before the tick, and setting *during* a tick
would violate the same-step laws (L1/L2). Each `(example, region)` gets its own
`conc::spsc` command queue and reply queue — the producer and consumer of each
direction is a single thread, so the SPSC contract holds even though one
program has three regions — and no new lock-free structure is introduced.

The command queue and the reply queue are **independently paced**: the tick
side can run many boundaries between two drains of the uWS loop (the demo's
audio thread ticks every few milliseconds, replies ship at 30 Hz), so "one
reply per command, therefore equal capacities suffice" would be wrong — a hot
region would out-run the reply queue and either abort the host (debug) or lose
Acks (release). Instead each boundary only consumes as many commands as the
reply queue has free slots (an exact reserved-slot counter, incremented by the
tick thread before it posts and decremented by the loop thread after it ships);
the rest stay queued and are answered at a later boundary. One Ack per accepted
command is therefore a guarantee — under a burst a reply may be a few timer
periods late, but it is never silently lost.

Enqueue-time overflow is different: a **full command queue drops** the command,
counts it, and never acks it (nobody will ever apply it). The cumulative count
is broadcast as `CommandStats { dropped }` whenever it moves (a duplicate is
never sent; a client that reconnects still learns the current value on
connect), so a client can tell "my request id will never be answered" from
"still queued / waiting behind reply backpressure".

Protocol (additive, `proto/clockr_debug.proto`; the JS page decodes only the
messages it knows and ignores the rest):

| Message | Direction | Meaning |
|---|---|---|
| `SetInput { request_id, port, Value value, optional example }` | client → server | set an input on the next boundary of its region |
| `ReadInput { request_id, port, optional example }` | client → server | read an input's value at the boundary |
| `ReadOutput { request_id, port, optional example }` | client → server | read the last completed tick's output |
| `Ack { request_id, error, applied_epoch }` | server → one client | answers `SetInput`; `error` is empty on success |
| `PortValue { request_id, port, Value value, error }` | server → one client | answers a read |
| `CommandStats { dropped }` | server → all | cumulative command-path loss |

Replies are routed to the requesting connection only (request ids are only
meaningful there). Validation happens at enqueue time on the loop thread
against the example's graph description, so a bad name costs the ticking thread
nothing and is answered immediately: an unknown port, a wrong-kind port (e.g.
`SetInput` on an output), a non-active explicit `example` name (per-example
addressing is deferred), or an unsupported value kind are all error `Ack`s with
`applied_epoch = 0` and no queue entry. Chunk A accepts the scalar kernel
payloads (`unit`/`bool`/`int`/`double`) for injection; user structs and text
are rejected with an explicit "unsupported value kind for port ..." message
(injecting a struct needs a runtime constructor from its schema, which only a
typed host setter can provide — deferred). *Reading* struct/text ports works:
the reply uses the same schema-driven encoding as the trace.

### The host contract

The host attaches **once**: its tick loops never change. Build one
`debug::command_hook` over an example's command queues and hand it to every
engine that ticks that example:

```cpp
debug::example_commands& commands = *server.commands_for("my-example");
debug::command_hook hook { commands };

kernel::debug_engine engine { recorder };   // or kernel::release_engine
engine.set_step_hook(&hook);

// ... forever after, exactly this call — the same line for either engine type:
engine.step(program, clock);
```

Why a kernel hook instead of wrapping `step` at the call site: the engine type
is a *construction-time choice*. A host switches between `debug_engine` and
`release_engine` — or builds the debugger in or out — by changing one
construction line, and its tick loops must not know which engine they hold.
The `kernel::step_hook` seam (`engine::set_step_hook`) keeps the loop exactly
`engine.step(program, clock)` while keeping injection at the tick boundary,
never inside evaluation, so the same-step laws (L1/L2) still hold. Attaching
nothing costs two predictable branches per step and no allocation.

How the hook sees a tick:

- `before_step` runs as the first statement of `engine::step`, before the
  affinity guard and before reclamation. It drains that region's queued
  commands: `SetInput`s become program state the tick is about to consume
  (ordinary between-tick state, semantically identical to a host `set`), and
  reads are answered from the same boundary snapshot.
- `after_step` runs exactly once. On a clean tick it is after the epoch is
  published; on a throwing tick it runs after the region-end trace event but
  before the exception propagates, with `threw = true` and the *attempted*
  epoch (a throwing tick publishes none) — that is when the Acks go out,
  because the command was consumed either way.
- Hooks must be cheap and must not throw: they run on the ticking thread and
  the after hook also runs on an unwind path.

The queue object belongs to the server. A host fetches it with
`server.commands_for(name)` and keeps the reference while the example is
registered; it is stable until `remove_example(name)` returns — the same
lifetime contract as the recorder reference the example holds. A host with a
*dynamic* example set must re-fetch after every `add_example` and must drop
the reference before removing that example (using it afterwards is a
dangling-queue use, exactly like using the recorder after removal).
`debugger_demo` resolves its reference once at startup because it never
removes an example — that is a demo-specific assumption, not part of the
server contract. The debug-build asserts inside the drain/finish pair catch
hook misuse (wrong thread, two drains without a finish, more than one step
between them).

`debug::command_drain` and `debug::command_finish` remain the primitives the
hook is built from — a host with a hand-written scheduler may call them
directly, and the tests pin them — but the normal path is attach-once. The
uWS handlers are thin wrappers over `server::handle_client_frame` and
`server::drain_commands`, so the network path is testable without a socket.

### Agent path

The write-back path exists so a software agent can close the loop: observe the
trace, change an input, and see the effect. Three layers sit on top of this
server:

- **`client/`** — `clockr-debug-client`, the zero-dependency TypeScript
  protocol client (node ≥ 24 runs it directly; no install): live events with
  `sinceSequence` replay, `setInput`/`getInput`/`getOutput`, example
  selection, pause/resume. Usage and API in `client/README.md`.
- **`mcp/`** — `clockr-debug-mcp`, a zero-dependency stdio MCP server over
  that client: ten tools (`graph_get`, `events_tail`, `events_watch`,
  `example_list`, `example_select`, `input_set`, `input_get`, `output_get`,
  `pause`, `resume`) plus the `graph://<example>` resource. Run, test and
  protocol subset in `mcp/README.md`.
- **`doc/how-to/agent-live-debug.md`** — the agent workflow end to end: build
  and run `debugger_demo`, wire the MCP server into an agent (copyable
  OpenCode config), drive the app, diagnose from events, fix and re-verify.
  The wiring is config-only; no agent internals are touched.

The acceptance bar the whole road must clear: **the agent receives events
while I drive the app; the agent sets an input; the traced stream shows the
effect; the agent reads it back.** It is scripted at each layer — run these
from the repo root, no install needed:

```bash
# C++ server + codec + command path (38 tests; needs a configured debugger build)
ctest --test-dir _build/<preset> -R debugger

# client: codec, proto/web sync, socket lifecycle, live e2e (221 checks)
(cd tools/debug-client && node test/codec.test.ts && node test/sync.test.ts \
  && node test/client.test.ts && node test/e2e.test.ts)

# MCP: protocol suite (mock client) and the live stdio e2e (220 + 122 checks)
(cd tools/debug-mcp && node test/protocol.test.ts && node test/e2e.test.ts)
```

Both e2e runs spawn `debugger_demo` themselves on a free port in 19000–19999
(override `DEBUGGER_DEMO=/path/to/debugger_demo`); with the binary absent they
print `SKIP` and exit 0, so the fast suites run without a C++ build. `npm test`
and `npm run test:all` in each package are the equivalents.

Independent verification of this path caught a real burst defect after the
first landing: a rapid `SetInput` flood could overflow the reply queue and
abort the demo, because a hot region consumes many commands between the uWS
loop's reply drains. `command_drain` now applies exact reserved-reply
backpressure — a boundary consumes only the commands whose replies fit and
leaves the rest queued, so an accepted command is acked exactly once (late,
never silent) and a dropped one is never acked. The flood accounting is exact
(3000 frames → 1841 acked + 1159 dropped) and the regression is pinned by
`debugger.injection.reply_backpressure`; the lesson is in `GOTCHAS.md`.

A fourth package, `tools/debug-dsh-plugin`, wraps the same client as one
Cordis service (`ctx.clockrDebug`) for DeepSeek Harness. It is a **preview
scaffold** only: source, not published, not booted in a real dsh process, and
its README lists what is verified versus preview-risky; its fake-context test
runs with `cd tools/debug-dsh-plugin && node test/plugin.test.ts` (69 checks).
The MCP server is the shipped agent surface.

## Tests

```
ctest --test-dir _build/<preset> -R debugger
```

- `debugger_tests` — the C++ codec round-trips and the user-type schema/value
  encoding (`message.test.cpp`), the recorder's ordering, drop accounting,
  capture gate and user-value retention (`recorder.test.cpp`), the server's
  example registry and drain paths (`server.test.cpp`), and the write-back
  command path (`command.test.cpp`): the command/reply schema round-trip, a
  boundary application whose Ack epoch matches the traced `REGION_EVAL_END`,
  enqueue-time validation (unknown/wrong-kind/non-active commands are error
  Acks with no queue entry), visible drop accounting (`CommandStats`) with
  dropped commands never acked, and the queued-reply reference hand-back. The
  four plan-mandated cases are `debugger.schema.setinput`,
  `debugger.injection.boundary`, `debugger.injection.unknown_port` and
  `debugger.injection.drop_visibly`; the post-landing cases are
  `debugger.injection.reply_backpressure`, `debugger.injection.cold_region`
  and `debugger.injection.detach` (clearing the step hook leaves commands
  queued and unacked), and `injection.boundary` also drives a
  `release_engine` with no trace configured to pin the
  engine-type-invariant tick loop. The kernel hook itself is covered by
  `kernel | A step hook brackets every tick exactly once` and
  `kernel | Detaching the step hook stops the callbacks` in
  `src/kernel/tests/engine.test.cpp`.
- `debugger.schema.smoke` — the JS decoder against hand-built frames.
- `debugger.layout.smoke` — the JS layout against a representative graph,
  asserting no overlapping boxes, one disjoint band per clock and routed
  edges (it loads the vendored ELK).

There is no automated DOM test; the demo is the manual acceptance vehicle.
The demo's network exercises all three regions, a `latest` and a `queued`
crossing, `hold`/`delay` state, a described user struct crossing a clock
(rendered inline with its fields), and a lift that throws every
`--throw-every N` ui ticks (red ticks to browse). A fresh client connecting
mid-stream gets the graph and a replayed tail.

## Options

```
debugger_demo [--host ADDR] [--port N] [--web-root DIR] [--throw-every N]
```

`--host` defaults to `127.0.0.1`: a debugger has no business on the LAN, and
embedded hosts must not expose a port beyond the machine.

The page connects to `ws(s)://<host>/ws` by default. A deployment that hosts
it apart from the native server can override the endpoint in `config.js`
(`window.DEBUGGER_WS_URL`) or per visit with `?ws=wss://…`.

## Development notes

`GOTCHAS.md` records the problems hit while building this tool (protobuf and
abseil build fights, proto3 packing/omission rules the page must follow, the
timeline layout) and how each was solved.
