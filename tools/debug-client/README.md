# clockr-debug-client

A dependency-free TypeScript client for the clockr debug WebSocket protocol
(`clockr.debug.v1`). It is the observation-and-injection half of the
debug-agent tooling: attach to a host serving `tools/web-debugger`, stream its
live evaluation trace, and write values back into it through the server's
tick-boundary command queue.

It is deliberately small and self-contained:

- **No build step.** Node ≥ 24 runs these `.ts` files directly (type
  stripping), so there is no bundler, no `tsc`, and nothing to `npm install`.
- **No runtime dependencies.** The protobuf codec is hand-rolled in this repo
  (stage-copied from the web UI, see [Codec and sync policy](#codec-and-sync-policy)).
- **No MCP/host coupling.** The MCP server and the harness plugin preview are
  thin wrappers over this package; all protocol knowledge stays here.

The package is the client the debugging tools are built on. If you are here to
write a tool on top of the debugger — an agent plugin, a script, a CI probe —
this is the API to build against.

## Requirements

- Node **v24.16 or newer** (any Node ≥ 22.18 that strips TypeScript types
  without a flag works; `WebSocket` is a Node global since 22).
- A debug host: today that is `debugger_demo` from `tools/web-debugger` (build with
  `CLOCKR_BUILD_DEBUGGER=ON`, see [`../web-debugger/README.md`](../web-debugger/README.md)).

The client never touches the C++ tree, CMake, or the web page.

## Quick start

Start a host (the demo's websocket endpoint is `ws://127.0.0.1:9002/ws`):

```bash
./_build/<preset>/tools/web-debugger/debugger_demo
```

Then, from this directory:

```js
// hello-debug.mjs — run with: node hello-debug.mjs
import { createClient } from "./src/index.ts";

const client = createClient("ws://127.0.0.1:9002/ws");

const graph = await client.connect();

console.log(`${graph.name}: ${graph.nodes.length} nodes`);
console.log("inputs:", graph.inputs.map((port) => port.name).join(", "));

// Read the live trace for a moment: this yields the connect-time history tail
// first, then batches as they arrive. `dropped > 0` means the producer had to
// throw a trace event away (the trace is visibly lossy, never silently gappy).
const stop = AbortSignal.timeout(1000);

for await (const batch of client.events({ signal: stop })) {
  for (const event of batch.events) {
    if (event.kind === "node_tick_end" && event.value?.kind === "double") {
      console.log(`epoch ${event.epoch} node ${event.nodeId} = ${event.value.value}`);
    }
  }
}

// Write back: applied at the next tick boundary of the owning region. The ack
// names the epoch that consumed the value; there is no way for an ack to be
// lost silently (see "Never lose an ack" below).
const ack = await client.setInput("bpm", 123.0);

console.log(`applied bpm at epoch ${ack.appliedEpoch}`);

// Read back: snapshots are taken by the host's tick loop too.
console.log("bpm is now", (await client.getInput("bpm")).value);

client.close();
```

While the demo is live its scripted driver overwrites most inputs every ui
tick, so `getInput` may see the driver's value rather than the injected one.
Prove an injection by its traced effect, or use a host whose driver leaves the
port alone when you need a deterministic read-back (see [Tests](#tests)).

## API

Everything below is exported from `src/index.ts` (the package root). The API
is frozen for the MCP server; breaking it needs a coordinated change.

### `createClient(url = "ws://127.0.0.1:9002/ws", options?): DebugClient`

The client opens lazily: nothing connects until `connect()` is called.

`options` (all optional):

| option | default | meaning |
|---|---|---|
| `reconnect` | `true` | reopen automatically after an unexpected socket close |
| `reconnectBaseDelayMs` | `100` | first backoff delay; doubles per attempt |
| `reconnectMaxDelayMs` | `2000` | backoff cap |
| `requestTimeoutMs` | `5000` | how long a command waits for its ack/port value |
| `connectTimeoutMs` | `10000` | how long the first `connect()` waits for the graph |
| `historyLimit` | `10000` | events retained per clock for `sinceSequence` replay |
| `replaySettleMs` | `150` | quiet window that ends the connect-time replay |
| `webSocketFactory` | global `WebSocket` | test seam: construct the socket yourself |
| `debug` | silent | `(line) => void` protocol log |

### Connection and observation

| member | description |
|---|---|
| `connect(): Promise<GraphDescription>` | open the socket (idempotent) and resolve with the first graph; auto-reconnect afterwards |
| `close(): void` | close the socket, stop reconnecting, reject every pending request |
| `state` | `"idle" \| "connecting" \| "open" \| "reconnecting" \| "closed"` |
| `graph` | latest `GraphDescription` (null before the first arrives) |
| `currentExample` | the live example name (graph name, else the examples list's active) |
| `examplesList` | latest `ExampleList` (null before the first arrives) |
| `cursors` | `Record<clock, bigint>`: highest sequence accepted per clock — feed back as `sinceSequence` |
| `dropped` | `{ events: Record<clock, bigint>, commands: bigint }` — loss accounting |
| `decodeErrors`, `orphanAcks`, `pendingRequestCount` | counters a diagnostics layer can surface |
| `onGraph(cb)`, `onEventBatch(cb)`, `onExamples(cb)`, `onConnection(cb)`, `onStats(cb)` | subscribe; each returns an unsubscribe function |

`onGraph` fires for every graph description, including the one a reconnect
resends and the one an example switch broadcasts. `onEventBatch` receives every
accepted (deduplicated) batch, `{ clock, dropped, events, replay }`.

### `events(options?): AsyncGenerator<EventBatchView>`

The read side of the debug loop. Each yielded batch is
`{ clock, dropped, events, replay }`:

- `clock` is the producer thread (`"audio" | "ui" | "worker"`), per-thread
  ordering is a total order on `sequence`;
- `dropped` counts trace events the producer's bounded queue could not accept
  since the previous batch on that clock;
- `replay: true` marks the connect-time history tail.

Options:

| option | default | meaning |
|---|---|---|
| `sinceSequence` | client read cursor | `Record<clock, bigint\|number>`: yield events with `sequence > since[clock]` |
| `live` | `true` | after the history replay, keep streaming; `false` ends when the replay settles |
| `signal` | — | `AbortSignal` to end iteration |

The iterator first yields the client's retained history for the requested
cursor, then live batches. With no `sinceSequence`, it continues from the
client's **read cursor** — the highest sequence already yielded to any
`events()` consumer — so the second call picks up exactly where the first
stopped. Pass explicit cursors to replay from the retained window (or from the
server's connect-time tail).

Reconnect is transparent to a live iterator: the server replays its history
tail on the new connection, the client suppresses sequences it already
delivered, and the missed tail is yielded normally.

### Commands: write-back and read-back

| member | description |
|---|---|
| `setInput(port, value, options?): Promise<{ requestId, appliedEpoch }>` | enqueue a write; resolves when the host applied it at a tick boundary |
| `getInput(port, options?): Promise<PortValue>` | snapshot an input at a tick boundary |
| `getOutput(port, options?): Promise<PortValue>` | snapshot the value an output produced on its region's last tick |
| `examples(): Promise<ExampleList>` | cached example set, waiting for the first list if needed |
| `selectExample(name): Promise<void>` | switch the live example; resolves when the new graph arrives |
| `pause(): Promise<void>` / `resume(): Promise<void>` | gate capture (an idle debugger costs the host nothing) |

`options` is `{ timeoutMs?, example? }`. `example` is passed through on the
wire; **the debug server answers only the active example** and rejects any other name
with an error ack (per-example addressing is deliberately deferred, but the
field is already on the wire so it will not break clients later).

`PortValue` is `{ requestId, port, value, error }`; an `error` text rejects the
promise instead of resolving.

#### Never lose an ack

Every request is tracked in a pending map keyed by its request id and always
ends in exactly one of:

- **resolve** — the ack (with `appliedEpoch`) or the port value arrived;
- **`AckError`** — the server answered with an error text (`requestId`,
  `ackError`, `port` on the error object; message text is the wire error);
- **`RequestTimeoutError`** — nothing arrived in `requestTimeoutMs` (the host
  may have no ticker yet; a dropped command is *never* acked, so this is also
  how a command-queue drop surfaces — combined with `dropped.commands`);
- **`NotConnectedError` / `ClientClosedError`** — the request never left, or
  the client was closed.

`appliedEpoch !== 0n` on a successful write means the host applied the value
at that region epoch. A command that never reached a boundary is an error ack
with `appliedEpoch === 0n`.

Supported injected value kinds are the scalars the host can materialize:
`unit`, `bool`, `int`, `double`. Chunk A rejects `struct` and `string` writes
with an error ack and a message naming the port (the kernel has no runtime
constructor from a schema); the codec still round-trips them, so the wire stays
forward-compatible.

### Values

The kernel's value variant gets a JSON-friendly mapping. Decoders always return
this canonical form:

```js
{ kind: "unit" }                                   // monostate / no event
{ kind: "bool",   value: true }
{ kind: "int",    value: 42n }                     // int64 — bigint, never truncated
{ kind: "double", value: 120.0 }
{ kind: "struct", value: { typeId: 7, fields: [...] } }
{ kind: "string", value: "saw" }
{ kind: "absent" }                                 // an unset oneof on the wire
```

**64-bit integers are `bigint` everywhere** — `sequence`, `stampNs`, `epoch`,
`nodeId`, `requestId`, `appliedEpoch`, `dropped` and `Value.int` — because an
agent must not silently truncate a kernel `int64` or a sequence frontier. The
two exceptions are the `uint32` ids (`TypeSchema.id`, `FieldSchema.typeId`,
`StructValue.typeId`), which are numbers.

`setInput` also accepts sugar, so the common case needs no union:

```js
client.setInput("bpm", 120.0);          // number → double
client.setInput("trigger", 7n);         // bigint → int
client.setInput("playing", true);       // boolean → bool
client.setInput("label", "saw");        // string → string (encoded; host rejects)
```

A plain `number` is **always a double** — `int64` needs `bigint` to stay
lossless. Use `{ kind: "int", value: 42n }` (or `42n`) for an integer.

For the JSON/stdio boundary (MCP), `valueToJson(value)` and
`bigintToJson(value)` convert without `JSON.stringify` throwing on bigint:
ints become numbers when lossless, decimal strings otherwise.

## Codec and sync policy

`src/protobuf.ts` and `src/schema.ts` are **stage-copied** from the web UI's
hand-rolled codecs (`tools/web-debugger/web/protobuf.js`, `web/schema.js`). The web
page must stay bundler-free, so it cannot import TypeScript; the package vendors
a copy at first land, and a follow-up will lift the codec into the package
once the page has a plain-JS build path.

Rules:

1. **`tools/web-debugger/proto/clockr_debug.proto` is the wire contract.**
   Never renumber or reuse a field; the schema grows additively within
   `clockr.debug.v1` (the package name is the version). New messages get a
   `WIRE_SCHEMA` entry in `src/schema.ts`.
2. **`web/schema.js` is the source of truth for the shared v1 messages.**
   When it changes, update `src/protobuf.ts`/`src/schema.ts` in the same
   commit — and vice versa for a shared fix.
3. Two deliberate client divergences, both documented in `src/schema.ts` and
   pinned by `test/sync.test.ts`: 64-bit fields decode as `bigint` (the page
   uses `number`), and `GraphDescription` field 2 is called `crossings` (the
   proto's name) where the page calls it `sharingPoints`.

`test/sync.test.ts` makes drift fail a test rather than a debugging session:

- it parses `proto/clockr_debug.proto` **as text** and compares every
  message field number and `EventKind` value with `WIRE_SCHEMA`, in both
  directions (an unpinned proto message fails too — that is the point);
- it decodes a hand-built corpus of shared v1 frames through both the web
  decoder and this package's decoder and requires identical results (after
  normalizing the two documented divergences);
- it byte-compares `pause`/`resume`/`select_example` encoding against the web
  decoder's encoders.

The client codec itself is covered by `test/codec.test.ts`, which hand-builds
wire frames from the proto (not through the decoder), checks byte-exact client
frames, and round-trips every `Value` kind — including `int64` extremes.

## Tests

No test framework, no install — plain node ESM files with a small assertion
harness (`test/support/harness.ts`). Each file is its own runner and exits
non-zero on failure:

```bash
node test/codec.test.ts    # codec unit tests: frames, values, encoders (86 checks)
node test/sync.test.ts     # codec ↔ proto ↔ web-decoder sync (36 checks)
node test/client.test.ts   # socket behaviour on a fake WebSocket (64 checks)
node test/e2e.test.ts      # spawns debugger_demo, exercises the whole loop
```

`npm test` runs the three fast suites (`test:codec`, `test:sync`,
`test:client` scripts exist too); `npm run test:all` adds the e2e run (no
`npm install` needed — `node` runs the TypeScript directly).

### The end-to-end harness

`test/e2e.test.ts` spawns `debugger_demo` on a free port in **19000–19999** and
asserts the acceptance bar of the whole road:

- a graph and the example list arrive;
- trace events flow for several clocks, with known kinds, strictly increasing
  per-clock sequence numbers, and values;
- `setInput("bpm", 123.0)` gets an ack with `applied_epoch > 0` and the value
  shows up in the traced stream (the demo's `tempo` node evaluates to `246`);
- `getOutput("tempo")` reads back;
- `pause()` stops trace events and `resume()` restarts them;
- drop accounting is surfaced consistently (`dropped.events` equals the sum of
  the yielded batches; nothing silently lost).

Default binary path: `_build/linux_x64_debug/tools/web-debugger/debugger_demo`
(override with `DEBUGGER_DEMO=/path/to/debugger_demo`). If the binary does not
exist the harness prints `SKIP` and exits 0, so the unit suites can run without
a C++ build. Set `E2E_DEBUG=1` for protocol logs.

Two practical notes from building this harness:

- **Do not parse the demo's stdout for readiness.** When stdout is a pipe the
  demo's `std::cout` is block-buffered, so `"clockr debugger listening on …"`
  can be arbitrarily delayed (the C++ side writes `"\n"`, not `std::flush`).
  The harness polls the TCP port instead — the only reliable readiness signal
  for a spawned host.
- The harness `SIGKILL`s the host it spawned; the server binds its port
  exclusively, so never point a harness at a port in use.

## Compatibility notes

- **Protocol:** `clockr.debug.v1`, additively versioned. Unknown
  `ServerMessage` bodies are ignored by design — a newer server does not break
  an older client; new messages get a `WIRE_SCHEMA` entry and a sync-test pass.
- **Version pinning:** decode one frame per WebSocket message, no length
  prefix; `trace_event.sequence` is monotonic only *within one clock* and is
  reset by a different example's engine.
- **Active-example scope (active-example scope):** commands target the live example only.
  The optional `example` field travels but is rejected if it names another
  example.
- **Injection value kinds:** scalar (`unit`/`bool`/`int`/`double`) write-back is
  supported; struct/text write-back is deferred (error ack). Trace *values*
  include structs and strings, decoded fully.
- **Reconnect/restart:** a dropped socket reopens automatically with backoff and
  the missed tail is recovered from the server's history replay. A *restarted*
  server (same example name) is detected when replayed sequence numbers
  disagree with the retained `stamp_ns`; the frontier is then reset. Sequences
  older than the retained history window cannot be compared, so a restart
  followed by a long replayed tail can repeat a bounded part of that tail —
  the protocol has no session id to do better. `historyLimit` and
  `replaySettleMs` are the client-side knobs.
- **`CommandStats.dropped` is cumulative** for the server's active example
  (broadcast when it moves); `dropped.commands` is the latest value, not a sum.
  `EventBatch.dropped` is per batch, so `dropped.events` sums per clock.
- **Host contract:** the host builds one `debug::command_hook` over that
  example's queues (`server::commands_for(name)`) and attaches it once to
  every engine that ticks the example (`engine::set_step_hook`). The hook's
  `before_step` drains and applies the queue, `after_step` acks with the
  consumed epoch; the tick loop stays plain `engine.step(program, clock)`, so
  the engine type (debug or release) remains a construction-time choice.
  Without a ticking owner a command is never applied — and therefore never
  acked; the client times out cleanly.

## Layout

```
src/
  index.ts      public entry point (the frozen API surface)
  client.ts     DebugClient, createClient, errors, options
  types.ts      decoded wire types (GraphDescription, TraceEvent, Value, …)
  value.ts      Value mapping, input sugar, JSON conversion
  protobuf.ts   stage-copied proto3 wire reader (web/protobuf.js)
  schema.ts     stage-copied web schema + v1 injection additions + WIRE_SCHEMA
  wire.ts       minimal proto3 wire writer (encoder primitives)
test/
  codec.test.ts   wire format unit tests
  sync.test.ts    proto/web/codec drift guard
  client.test.ts  fake-WebSocket lifecycle tests (reconnect, acks, replay)
  e2e.test.ts     spawn debugger_demo, full debugging loop
  support/        assertion harness, hand-built server frames, fake socket
```

## See also

- [`../web-debugger/README.md`](../web-debugger/README.md) — the debugger itself (server, web page, demo,
  protocol).
- [`../web-debugger/GOTCHAS.md`](../web-debugger/GOTCHAS.md) — problems hit
  building the debug pipeline.
