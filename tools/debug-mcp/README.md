# clockr-debug-mcp

A **zero-dependency stdio MCP server** for live-debugging a running `clockr`
host: it exposes the debug WebSocket protocol (`tools/web-debugger`) as Model
Context Protocol tools and resources, so a coding agent can watch the live
evaluation trace, write inputs back at tick boundaries, and read the traced
effect of what it just did.

It is the agent-facing layer of the debugging stack:

```
host tick loop ──▶ debugger server ──▶ WebSocket ──▶ clockr-debug-client
                                                              │
                                                     clockr-debug-mcp
                                                              │
                                                     stdio MCP (JSON-RPC)
                                                              │
                                                       the agent (OpenCode,
                                                       DSH, any MCP client)
```

Nothing here re-implements the wire protocol: the MCP layer imports the
client (`../../debug-client/src/index.ts`) and stays a translation layer —
validate JSON arguments, call one client method, serialise the result.

The user-facing workflow this enables is
[`doc/how-to/agent-live-debug.md`](../../../doc/how-to/agent-live-debug.md);
this file is the package's own reference (run, test, protocol subset,
compatibility).

## Run it

```bash
# 1) start the host (any host embedding debug::server works; the demo is easiest)
./build.sh
_build/<preset>/tools/web-debugger/debugger_demo

# 2) run the MCP server by hand to check the wiring
node tools/debug-mcp/src/main.ts --url ws://127.0.0.1:9002/ws
```

The server speaks newline-delimited JSON-RPC 2.0 on stdin/stdout and does
nothing until a client talks to it. It connects to the debugger in the
background and keeps retrying, so **start order does not matter**: an MCP tool
call that needs the debugger simply waits for the socket.

An MCP client points at the installed command:

```json
"mcp": { "clockr": { "command": "npx", "args": ["clockr-debug-mcp"] } }
```

This repository ships the package **source** (npm publish is deferred, see
[Compatibility](#compatibility)); until it is published, use the local form:

```json
"mcp": {
  "clockr": {
    "command": "node",
    "args": ["tools/debug-mcp/src/main.ts", "--url", "ws://127.0.0.1:9002/ws"]
  }
}
```

### Options

```
clockr-debug-mcp [options]

  -u, --url <ws-url>        debugger WebSocket (default ws://127.0.0.1:9002/ws;
                            env CLOCKR_DEBUG_URL)
      --debug               log protocol/connection activity to stderr
                            (env CLOCKR_DEBUG_MCP_DEBUG=1)
      --client-module <p>   test seam: load an alternate client factory
  -h, --help                usage
  -V, --version             version
```

`stdout` is the protocol channel — everything else (including `--debug`
output) goes to stderr.

## What it exposes

### Tools

| Tool | Arguments | Result (JSON) |
| --- | --- | --- |
| `graph_get` | — | `{example, counts, nodes, crossings, inputs, outputs, types}` |
| `events_tail` | `n` (1–2000, default 50), `since?`, filters (`clock`/`nodeId`/`kind`), `maxEvents?` (1–2000, default min(n,200)), `maxBytes?` (default 262144), `summary?` | `{count, matched, truncated, events, dropped, cursors, replay, nextSince}` (summary mode: `summary`, no `events`) |
| `events_watch` | `since?`, `timeout` (ms, default 2000), filters (`clock`/`nodeId`/`kind`), `maxEvents?` (1–2000, default 200), `maxBytes?` (default 262144), `summary?` | `{count, matched, truncated, timedOut, events, dropped, cursors, nextSince, since, autoPaged}` (summary mode: `summary`, no `events`) |
| `example_list` | — | `{active, names}` |
| `example_select` | `name` | `{selected, active, graph: {nodes, inputs, outputs}}` |
| `input_set` | `port`, `value`, `timeoutMs?` | `{port, requestId, appliedEpoch}` |
| `input_get` | `port`, `timeoutMs?` | `{requestId, port, value}` |
| `output_get` | `port`, `timeoutMs?` | `{requestId, port, value}` |
| `pause` | — | `{paused: true}` |
| `resume` | — | `{paused: false}` |

Every result is a single MCP `text` content block containing **one JSON
document**, so an agent can parse it deterministically. A real failure of the
debugger (unknown port, request timeout, unknown example, not connected) is
returned as an MCP tool result with `isError: true` and
`{"error": "...", "code": "<client error code>"}`; the model can read it and
try something else. Only malformed *arguments* are JSON-RPC errors
(`-32602`), because retrying those cannot work.

**`events` shape.** Each event is
`{sequence, stampNs, kind, clock, nodeId, epoch, value, threw, durationNs}`;
`value` is `null` for begin events. `dropped` is
`{events: {<clock>: <count>}, commands: <count>}` — a lost trace event or a
lost injection command is always visible, never silent. `count` is how many
events were returned, `matched` how many passed the filters (and were dropped
by a cap), and `truncated` says whether a cap dropped anything. `nextSince` is
the per-clock last **kept** sequence — the paging cursor (see
[Bounding event queries](#bounding-event-queries)). `cursors` is the client's
accepted frontier: it keeps working for the non-truncated case and for
"everything the client has seen", but see the warning below before paging with
it. `events_tail` with no `since` means "the retained tail", not "since my
last tool call", so it is reproducible for a fresh agent; `events_watch` with
no `since` starts at the current head.

**`events_watch`** returns as soon as there is a delta — the retained delta
after `since` if one exists, otherwise the next live batch — plus a short
15 ms settle so one tick from every clock usually lands together. If nothing
arrives within `timeout`, it returns `count: 0, timedOut: true` (a normal
result, not an error). It stops consuming the stream as soon as `maxEvents` is
reached, and drops the newest events if the serialized result would exceed
`maxBytes`; either way the result is truncated and `nextSince` continues from
exactly the last event returned.

### Bounding event queries

A live trace is verbose (a single event can be hundreds of bytes once its
value is serialised), and a fresh connection replays retained history. The two
event tools therefore enforce caps **at the protocol level**:

| Knob | Default | Hard limit | Meaning |
| --- | --- | --- | --- |
| `maxEvents` | `events_tail`: `min(n, 200)`; `events_watch`: 200 | 1–2000 | Maximum events in the result |
| `maxBytes` | 262144 (256 KiB) | 1024–2097152 (2 MiB) | **HARD** limit on the serialized result text (`Buffer.byteLength`) |
| `summary` | `false` | — | Aggregates only; no `events` array |

`maxBytes` is enforced exactly: the server builds the payload, measures the
serialized text, and trims until it fits (`truncated: true` reports the loss).
`events_tail` trims from the **front**, so the newest events survive; a
truncated `events_watch` trims from the **end**, keeping the oldest prefix so
its `nextSince` is contiguous.

**Filters** apply before the caps (and `matched` counts post-filter events):

```json
{"name":"events_tail","arguments":{
  "since":{"audio":0}, "clock":"ui", "kind":["node_tick_end","node_tick_throw"],
  "nodeId":"0", "n":200
}}
```

- `clock`: string or array; validated against the live graph's clocks when the
  graph is known (unknown name → `-32602`).
- `nodeId`: safe integer or decimal string (node ids are uint64), or an array
  of those.
- `kind`: string or array, from `region_step_begin`, `region_step_end`,
  `node_tick_begin`, `node_tick_end`, `node_tick_throw`.

**Summary mode** aggregates over the whole filtered (retained) trace for a few
hundred bytes — no per-event payloads, so it is a cheap way to ask "what is
happening?":

```json
{"name":"events_tail","arguments":{"summary":true,"clock":"ui"}}
```

produces, inside the normal result envelope:

```json
{"summary":{
  "events": 2410,
  "byClock": {"ui": {"events": 2410, "byKind": {"node_tick_end": 1900, "region_step_end": 30},
                     "epochs": {"first": 1, "last": 803}, "threw": 0}},
  "byKind": {"node_tick_end": 1900, "region_step_end": 30},
  "nodes": [{"nodeId": 5, "count": 800}],
  "epochs": {"first": 1, "last": 803},
  "threw": 0
}}
```

`summary` ignores `maxEvents` (it returns no events); its node histogram is a
bounded top-20 and is trimmed further if `maxBytes` demands it
(`truncated: true`). The totals (`events`, `byKind`, `byClock`, epochs,
`threw`) stay intact.

#### Paging: continuation is the default

A truncated `events_watch` has already moved the client's internal read cursor
past the events the cap trimmed away, so a naive next call **without** `since`
would silently skip them. The server therefore makes continuation the default:
it remembers the last truncated page's `nextSince` for the life of the process
(one stdio MCP server = one agent session) and uses it when `since` is omitted.

```json
{"name":"events_watch","arguments":{"since":{},"maxEvents":200}}
→ {"count":200,"matched":4812,"truncated":true,"autoPaged":false,
   "since":{},"nextSince":{"audio":8123,"ui":402}, …}

{"name":"events_watch","arguments":{"maxEvents":200}}        ← no since: continues
→ {"count":200,"matched":4612,"truncated":true,"autoPaged":true,
   "since":{"audio":8123,"ui":402},"nextSince":{"audio":8341,"ui":408}, …}
```

Repeat until `truncated` is false; after that, a no-since watch is a normal
live watch again. It is visible, not magic: `since` echoes the per-clock cursor
actually used (`{}` when the client's read head is the default) and
`autoPaged: true` means the omitted `since` came from the stored page.

While the host keeps ticking, a small page can stay `truncated` indefinitely:
new events arrive faster than the round trip, so the page never catches up.
That is not a leak — to get a finite snapshot, `pause` capture first (the trace
stops growing) or use a `maxEvents` comfortably above the tick rate. The same
applies to comparing two live scans (e.g. summary counts vs a scan): pause for
a stable history if the counts must match exactly.

An explicit `since` always wins: pass `{"since": {}}` to deliberately restart
from the retained history, or your own cursor to jump; the next truncated
result re-bases the stored cursor on that explicit call. The stored cursor is
cleared when a watch result is not truncated (caught up), on `example_select`
(it is keyed to the active example), and whenever the connection is not open.

If you prefer an explicit loop — e.g. you are writing a custom client rather
than driving the session by hand — `since: <previous result>.nextSince` is
equivalent and always works, and `nextSince` remains in every result.

**Do not page with `cursors`.** `cursors` is the client's accepted frontier,
which has already advanced over the events a cap trimmed away, so resuming
from it would silently skip them. `nextSince` is the last sequence actually
*returned*. `cursors` remains correct for the untruncated case and for asking
"what has the client accepted so far". `nextSince` is seeded from the effective
`since`, so clocks with no kept events do not advance; the next page re-reads
their retained history rather than skipping it.

(The auto-paging cursor is per-process session state: a restarted MCP server —
or an MCP client that reconnects to a new session — starts with no page, and a
`example_select` invalidates it. For a stateless protocol, pass `nextSince`
explicitly.)

**`input_set`** is the interesting one. The value is validated and sent to the
host's bounded command queue; the host's ticking thread applies it immediately
before its next step. The result's `appliedEpoch` is the epoch of the tick
that consumed it, so the trace events produced by that write have an epoch at
or after it. This is not a workaround for thread safety: in a clocked
synchronous program a value set between ticks is indistinguishable from one
set before the tick, and setting it *during* a tick would violate the
same-step laws (L1/L2 in
[`doc/reference/denotational-semantics.md`](../../../doc/reference/denotational-semantics.md)).
Read it back with `events_watch`/`events_tail`, or snapshot with
`input_get`/`output_get` (both answered by the host at a tick boundary too).

**A snapshot is not an assertion.** `output_get` answers with the last
completed tick's value, and a host driver may keep overwriting the input: the
demo's scripted driver writes `bpm` every 33 ms, so a `tempo` snapshot taken a
moment after `input_set("bpm", 123)` will *not* be 246 — that is correct
behaviour, not a lost write. Prove an injection on a moving program by its
traced effect (the `bpm` node's event value 123 and the `tempo` node's event
value 246 for the one tick that consumed it); use a port the driver does not
touch for a deterministic snapshot read-back.

### Resource

The graph is also addressable as a noun:

- `resources/list` → one resource, `graph://<active example>`, mimeType
  `application/json`;
- `resources/read` → the same JSON document as `graph_get`.

Reading an example that is not active is MCP error `-32002` (resource not
found) with an explanation: per-example addressing is deferred, so the server
is active-example-only. Switch with `example_select`, then re-list; the
resource moves with the live example.

### Value mapping at the JSON boundary

The client's `Value` union maps 1:1 into JSON, via the client's `bigintToJson` /
`valueToJson`:

| Kernel value | JSON |
| --- | --- |
| `unit` | `{"kind":"unit"}` |
| `bool` | `{"kind":"bool","value":true}` |
| `int64` | `{"kind":"int","value":42}` (number when lossless, decimal string otherwise) |
| `double` | `{"kind":"double","value":2.5}` |
| `string` | `{"kind":"string","value":"saw"}` (text/enum spelling) |
| user struct | `{"kind":"struct","value":{"typeId":7,"fields":[…]}}` |
| absent oneof | `{"kind":"absent"}` |

For `input_set`, the *input* side accepts the canonical mapping plus sugar:
a JSON number is a **double** (`112.0` is never an int64), a boolean is a
bool, a string is text, `null` is unit. To write an int64 losslessly use
`{"kind":"int","value":42}` (number or decimal string) — a JSON number would
be a double by construction. Struct/text injection is rejected by the host
with an explicit "unsupported value kind" error ack (a typed setter is
required; deferred — reads of struct/text ports work).

## MCP protocol subset

Hand-rolled, dependency-free, and deliberately small:

| Method | Behaviour |
| --- | --- |
| `initialize` | returns `protocolVersion`, `capabilities` (`tools`, `resources`), `serverInfo`, and workflow `instructions` |
| `notifications/initialized` | no-op |
| `ping` | `{}` |
| `tools/list` | the ten tools with JSON Schemas |
| `tools/call` | dispatch, argument validation, tool results |
| `resources/list` / `resources/read` | `graph://<example>` |
| `shutdown` | acknowledges; later requests answer `-32600` |
| `exit` (notification) | closes the debug client and exits 0 |
| anything else | JSON-RPC `-32601 Method not found` |

Accepted protocol revisions: `2024-11-05`, `2025-03-26`, `2025-06-18`. A
requested revision outside that set negotiates down to `2025-06-18` (MCP's
rule: the server answers with a version it supports; capabilities then bound
the session).

**Not implemented** (and not advertised, so compliant clients will not call
them): prompts, resource templates, resource subscriptions, completion,
logging, sampling, progress/cancellation notifications, `outputSchema` /
`structuredContent`, auth, and the HTTP/SSE transports. Stdio input accepts
both MCP newline-delimited JSON and LSP-style `Content-Length` framing (early
MCP drafts and some tooling use the latter); output is always newline-delimited
JSON. Buffers are parsed byte-wise, so non-ASCII text cannot desynchronise
`Content-Length`.

## Compatibility

- **Node ≥ 24**, no build step, **no dependencies**. Node 24 runs the
  TypeScript directly via type stripping; `erasableSyntaxOnly` in
  `tsconfig.json` keeps the sources strippable (no enums/namespaces/parameter
  properties).
- **Imports the client by relative path** (`../../debug-client/src/index.ts`).
  That is intentional: this repository ships `tools/debug-client` and
  `tools/debug-mcp` side by side, and **node refuses to type-strip `.ts`
  files under `node_modules`**. A published `clockr-debug-mcp` therefore
  needs a real dependency on a compiled/published `clockr-debug-client` (or
  a bundling step) — the npm publish is deferred; the local-run form above is
  the supported one in this tree.
- **Frozen client API.** The server programs against a structural
  `DebugClientLike` (`src/debug-client.ts`) with `connect`, `events`,
  `setInput`, `getInput`, `getOutput`, `examples`, `selectExample`,
  `pause`/`resume` and the `graph`/`cursors`/`dropped` reads — exactly the
  client's public API. The real client is imported only in `main.ts`; the test
  seam `--client-module` swaps in a mock.
- **Compatibility risk** is a wire-protocol change: if the host's
  `clockr.debug.v1` additions change, the client (not this package) is what
  needs updating.

## Tests

Plain node scripts, no framework, no install. From this directory:

```bash
node test/protocol.test.ts     # PASS: 220 ok, 0 failed
node test/e2e.test.ts          # PASS: 108 ok, 0 failed  (needs debugger_demo)
npm run test:all               # both
```

- `test/protocol.test.ts` spawns the real MCP server over stdio with the
  **mock client** (`test/support/mock-client.ts`, via `--client-module`) and
  pins the MCP layer: initialize handshake and version negotiation,
  `tools/list` (exact ten tools + JSON Schemas), unknown method/tool/argument
  error codes, malformed JSON (`-32700`), `Content-Length`-framed input,
  unknown notifications never answered, every tool's result shape, the graph
  resource, and `shutdown`/`exit` (exit code 0, clean stderr). It also pins the
  bounded-event contract on a small deterministic mock and on a large verbose
  one (`MOCK_LARGE=1`): `maxBytes`/`maxEvents` caps, trim direction
  (tail keeps newest, watch keeps the oldest prefix), byte-exact bounds,
  filters (including arrays and decimal-string node ids), summary counts
  against a non-summary scan, `nextSince` paging without duplicates or gaps, and
  the auto-paging default (a no-since watch continues a truncated page; explicit
  `since` overrides; caught-up and example-switch clear the stored cursor; the
  `since`/`autoPaged` echo is correct). No C++ needed.
- `test/e2e.test.ts` is the real loop: it picks a free port in 19000–19999,
  spawns `_build/linux_x64_debug/tools/web-debugger/debugger_demo` (override with
  `DEBUGGER_DEMO=…`), waits for **TCP readiness** (the demo's piped stdout is
  block-buffered), spawns this MCP server against it, then asserts graph →
  trace → `input_set("bpm", 123)` with a positive applied epoch → the traced
  stream shows `bpm 123` and `tempo 246` (2 × bpm) → snapshot read-back
  (`input_get`/`output_get` resolve; the demo driver overwrites `bpm`, so the
  value is not asserted stable — the traced effect is the assertion) → pause
  gates capture, resume restores it → `example_list` and the `graph://`
  resource follow the active example → `shutdown`/`exit` exits 0. The demo is SIGKILLed in a
  `finally`. Without a binary it prints `SKIP` and exits 0, so the protocol
  suite still runs on a machine without a C++ build.

## Layout

```
mcp/
├── src/
│   ├── main.ts          entry: CLI, client construction, stdio wiring, lifecycle
│   ├── server.ts        MCP/JSON-RPC dispatch (the protocol subset above)
│   ├── stdio.ts         framing reader (newline + Content-Length), writer, teardown
│   ├── tools.ts         the ten tools: schemas, argument validation, JSON conversion
│   ├── resources.ts     graph://<example> list/read
│   ├── debug-client.ts  DebugClientLike seam over the client (type-only imports)
│   └── errors.ts        JSON-RPC/MCP error codes
└── test/
    ├── protocol.test.ts     MCP layer with a mock client
    ├── e2e.test.ts          real debugger_demo + real client
    └── support/             harness, RPC client, demo spawner, mock client
```

## Limitations (by design, for now)

- **Active example only.** Tools and the graph resource address whatever
  example is live; per-example addressing (the deferred `example` field) is
  not exposed.
- **Scalar injection only** (`unit`/`bool`/`int`/`double`); structs and text
  are rejected with an explicit ack error. Reads work for every kind.
- **No state patching.** `delay`/`scan` internals are not writable (needs
  state-snapshot semantics), and there is no per-region stepping/breakpoints —
  both are deferred.
- **One process, one debugger connection**, started from `--url` (nothing
  multiplexes several hosts).
- **Capture is gated globally**, as it is in the web UI: `pause` stops the
  trace recorder, not the program.

## Related

- [`doc/how-to/agent-live-debug.md`](../../../doc/how-to/agent-live-debug.md) —
  the agent workflow this server exists for (build the host, wire an agent,
  diagnose, inject, verify) and the copyable OpenCode config.
- [`tools/debug-client/`](../debug-client/) — the WebSocket client this
  package wraps.
- [`tools/web-debugger/README.md`](../web-debugger/README.md) — the debugger
  itself: host contract, wire schema, web UI.
- [`tools/debug-dsh-plugin/`](../debug-dsh-plugin/) — the DeepSeek Harness
  (dsh/Cordis) wrapper of the same client, as **one service**
  (`ctx.clockrDebug`). Developer preview: source only, publish deferred,
  lifecycle adapter only — the protocol stays in the client.
