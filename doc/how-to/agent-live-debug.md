# Debugging a running app with an agent

`clockr` networks are *live*: a host ticks region after region, and every
epoch has a shape — which node evaluated, what value it produced, how long it
took, whether it threw. The debugger (`tools/web-debugger`) already lets a human
watch that stream in a browser. This page is about the other half: letting a
**software agent** watch the same stream and *write back*, so the two of you
can drive the same running program together.

Concretely, an agent that has this tool attached can answer questions like
"why did the audio region just throw?", "what is `tempo` once I set `bpm` to
123?", or "did the value I wrote actually reach a tick boundary, or did the
host overwrite it first?" — and it can answer them against the program you are
driving, not against a model of it.

The integration is one small package and one config stanza: a **stdio MCP
server** (`tools/debug-mcp/`, package `clockr-debug-mcp`) that wraps the
TypeScript debug client in the Model Context Protocol. The library gains one
small seam for it — `kernel::step_hook` on the engine — and the host attaches
the debugger's `debug::command_hook` once, at construction; its tick loops
stay `engine.step(program, clock)` whatever the engine type, and the web UI is
unchanged.

This page is a how-to: it takes you from "a program is ticking" to "an agent is
debugging it with me" in a worked session, then collects the limits and the
troubleshooting table. The reference material is elsewhere:
[`tools/web-debugger/README.md`](../../tools/web-debugger/README.md) for the debugger
itself, [`tools/debug-mcp/README.md`](../../tools/debug-mcp/README.md)
for the MCP server's protocol details, and
[`tools/debug-client/README.md`](../../tools/debug-client/README.md) for the
client package underneath it.

## The shape of the loop

There are three moves, and the whole toolset is built around them:

```
observe ──▶ inject ──▶ verify
   │                    │
 events_tail /          events_watch / output_get /
 events_watch           input_get
   │
 graph_get (what ports exist?)
```

The one design decision worth understanding is **where a write lands**. The
agent's `input_set` does not poke the program's memory from the server thread —
that would race a region mid-`step()` and break the same-step laws (L1/L2 in
[`../reference/denotational-semantics.md`](../reference/denotational-semantics.md)).
Instead the server enqueues a command, and the host's ticking thread drains that
queue **as the first thing its next `engine.step()` does** (the attached
`debug::command_hook`'s `before_step`). So:

- the write is applied exactly once, at a **tick boundary**;
- a value set between ticks is indistinguishable from one set before the tick
  (which is semantically why this is the right place, not merely the safe one);
- the acknowledgment names the **epoch** of the tick that consumed the write —
  the `appliedEpoch` an agent gets back from `input_set`.

Reads work the same way: `input_get` / `output_get` are answered by the host's
tick loop from a boundary snapshot, never by a server-thread peek. That is why a
read can take a moment (it waits for the region to tick) and why it reflects
"the last completed tick", not this instant.

## What you need

- a host that serves the debugger. The easiest is the demo debugger:
  `debugger_demo`, which runs the `demo` example on simulated
  `ui`/`audio`/`worker` clocks;
- **node ≥ 24** (the MCP package runs TypeScript directly; there is no build
  step and no `npm install`);
- an MCP client — [OpenCode](https://opencode.ai) is the one this repository
  targets, but any MCP client that can spawn a stdio server works.

## Step 1 — run a host

Build the debugger and run the demo:

```bash
./build.sh
_build/<preset>/tools/web-debugger/debugger_demo
```

It prints where it is listening and starts ticking:

```
[debugger_demo] serving the debug UI on http://127.0.0.1:9002 — open it in a browser; Ctrl-C to stop
```

You do not have to open the browser — but it is worth doing once, because the
human gets the same graph and timeline the agent reads as JSON. One flag is useful for
exercises: `--throw-every N` makes the demo's `gain` lift throw every N ui
ticks, which gives the trace a real error to find.

Any other host that embeds `debug::server` (a plugin, your own app) works the same way, provided it upholds the host contract: build one
`debug::command_hook` over that example's `server::commands_for(name)` queues
and attach it to every engine that ticks the example, via
`engine::set_step_hook`. The hook drains and acks at each boundary; the tick
loop itself stays `engine.step(program, clock)`. See
[`tools/web-debugger/README.md`](../../tools/web-debugger/README.md#the-host-contract).

## Step 2 — point the agent at it

Add the server to the MCP client's config. In OpenCode that is one stanza:

```json
"mcp": { "clockr": { "command": "npx", "args": ["clockr-debug-mcp"] } }
```

That is the shipped form and will be the normal one once the package is on npm
(publish is deferred, see §"What is not here yet"). In this repository the
package is source, so use the local form — it is the same server, started
directly:

```json
"mcp": {
  "clockr": {
    "command": "node",
    "args": ["tools/debug-mcp/src/main.ts", "--url", "ws://127.0.0.1:9002/ws"]
  }
}
```

If the MCP client does not run from the repository root, give both paths
absolutely (`…/tools/debug-mcp/src/main.ts`). `--url` defaults to
`ws://127.0.0.1:9002/ws`; override it with `--url` (or
`CLOCKR_DEBUG_URL`) when the host listens elsewhere.

Two practical notes:

- **Start order does not matter.** The server connects to the debugger in the
  background and keeps retrying; an agent that starts before the host exists
  simply waits on its first tool call.
- **The import is relative on purpose.** The package imports the client as
  `../../debug-client/src/index.ts`, from inside this repository. Node refuses to
  type-strip `.ts` files under `node_modules`, so this local-run form — not an
  installed copy — is what works today; the npm story is in the package README.

## Step 3 — the agent's first three calls

Ask the agent to look before it leaps. The first call is always `graph_get`:
it returns the lowered network — nodes with their combinator labels and clocks,
crossings with their policies, and most importantly the *boundary port names*
that every other tool takes.

```json
{
  "example": "demo",
  "counts": { "nodes": 16, "crossings": 7, "inputs": 4, "outputs": 5, "types": 1 },
  "nodes": [ { "id": 0, "kind": "input", "clock": "ui", "label": "input", "port": "bpm", "fanIn": [] }, … ],
  "inputs":  [ { "nodeId": 0, "name": "bpm" }, … ],
  "outputs": [ { "nodeId": 2, "name": "tempo" }, … ]
}
```

Then read the stream. `events_tail` returns the most recent events — bounded
by default to 200 events and 256 KiB of text — plus the counts that tell you
whether a cap bit (`matched`, `truncated`), the drop counters, and the
**cursors** / **nextSince** paging metadata:

```json
{
  "count": 50,
  "matched": 4812,
  "truncated": true,
  "events": [ { "sequence": 8123, "kind": "node_tick_end", "clock": "audio",
                "nodeId": 2, "epoch": 1920,
                "value": { "kind": "double", "value": 120 }, "threw": false,
                "durationNs": 3500 }, … ],
  "dropped": { "events": {}, "commands": 0 },
  "cursors": { "audio": 8123, "ui": 402, "worker": 88 },
  "nextSince": { "audio": 8123, "ui": 402, "worker": 88 }
}
```

For "tell me the next thing that happens", use `events_watch` with the paging
metadata from the previous result. It returns as soon as there is a delta (the
retained delta after `since`, or the next live batch) — or `{"count": 0,
"timedOut": true}` after the timeout, which is a normal answer, not an error.
When the previous result was **not** truncated, passing its `cursors` back as
`since` makes successive reads exactly adjacent: no overlap, no gap. When it
**was** truncated, you can simply call `events_watch` again **without**
`since` — the server continues the page for you and says so with
`autoPaged: true` (or pass `nextSince` explicitly) — see
[Bounding what you ask for](#bounding-what-you-ask-for).

Two more controls complete the observation side:

- `pause` / `resume` gate **trace capture** (the program keeps ticking). Pause
  when the agent needs a stable window to inspect — the timeline stops growing
  under it — and resume before expecting new events.
- `dropped` is the honesty counter. The trace recorder is lock-free and bounded:
  under load it *drops* events and counts them per clock (`dropped.events`), and
  a full command queue drops writes (`dropped.commands`). A dropped write is
  never acked. If `dropped` grows, the trace is a sample, not a recording.

## Bounding what you ask for

A live trace is verbose (one event with a described value can be hundreds of
bytes once serialised), and a fresh connection replays retained history. A
naive `events_tail` or `events_watch` could therefore return megabytes into an
agent's context. The MCP server enforces caps itself, so this is not something
you have to remember to do — but it shapes how you read:

| Knob | Default | Hard limit | What it does |
| --- | --- | --- | --- |
| `maxEvents` | `events_watch`: 200; `events_tail`: `min(n, 200)` | 1–2000 | Caps how many events a result carries |
| `maxBytes` | 262144 (256 KiB) | 1024–2097152 (2 MiB) | **HARD** bound on the serialized result text; the server trims until it fits |
| `summary` | `false` | — | Returns aggregates only (no events array) |

When a cap drops something the result says so: `matched` is how many events
passed the filters, `count` how many came back, and `truncated: true` means a
cap removed the difference. `events_tail` keeps the **newest** events when it
trims; a truncated `events_watch` keeps the **oldest** prefix and hands you
`nextSince` so the rest is one call away — nothing is silently skipped.

**Ask narrower instead of bigger.** The three filters apply before the caps
(they accept a single value or an array):

- `clock` — `"ui"`, `["ui", "audio"]` (validated against the live graph);
- `nodeId` — a node id or array of them (`graph_get` maps ids to labels);
- `kind` — `"node_tick_throw"`, `["node_tick_end", "node_tick_throw"]`, ….

```json
{"name":"events_tail","arguments":{"clock":"ui","kind":"node_tick_end","nodeId":"0","n":200}}
```

**Or ask for a summary.** When the question is "what is happening?" rather
than "show me every event", `summary: true` aggregates the filtered retained
trace into a few hundred bytes — counts by clock and kind, epoch spans, throw
count and a top-20 node histogram:

```json
{"name":"events_tail","arguments":{"summary":true,"clock":"ui"}}
→ {"count":2410,"matched":2410,"truncated":false,
   "summary":{"events":2410,
              "byClock":{"ui":{"events":2410,"byKind":{"node_tick_end":1900,"region_step_end":30},
                                "epochs":{"first":1,"last":803},"threw":0}},
              "byKind":{"node_tick_end":1900,"region_step_end":30},
              "nodes":[{"nodeId":5,"count":800}],
              "epochs":{"first":1,"last":803},"threw":0},
   "cursors":{…},"nextSince":{…}}
```

**Paging a truncated result is automatic.** A truncated `events_watch` has
already moved the client's internal read cursor past the events the cap cut
away, so the natural next call — the same one again, without `since` — would
silently skip them. The server closes that hole: it remembers the last
truncated page and continues from it when `since` is omitted.

```
events_watch {"since": {}, "maxEvents": 200}
→ {"count":200,"matched":4812,"truncated":true,"autoPaged":false,
   "since":{},"nextSince":{"audio":8123,"ui":402}}
events_watch {"maxEvents": 200}                              ← no since: continues
→ {"count":200,"matched":4612,"truncated":true,"autoPaged":true,
   "since":{"audio":8123,"ui":402},"nextSince":{"audio":8341,"ui":408}}
… repeat until "truncated": false
```

It is visible, not magic: `since` echoes the cursor actually used and
`autoPaged` says whether it came from the remembered page. After the result is
caught up (`truncated: false`) the next no-since watch is a normal live watch
again. An explicit `since` always wins — pass `{"since": {}}` to deliberately
start over from the retained history — and the remembered page is cleared when
you switch examples (it belongs to one example). If you are writing your own
client loop rather than driving a session by hand, `since: nextSince` is
equivalent and always works.

One honest limit: while the host keeps ticking, a small page can stay
`truncated` forever because new events arrive faster than the round trip. That
is the live trace, not a bug — `pause` capture for a finite snapshot, or page
with `maxEvents` comfortably above the tick rate. The same goes for comparing
two live scans (summary counts vs a filtered scan): pause first if the counts
must match exactly.

Why not `cursors`? `cursors` is the client's *accepted* frontier: it has
already moved past the events a cap trimmed away, so resuming from it would
silently skip them. `nextSince` is the last sequence the result actually
**returned**. For an untruncated result the two are equivalent; for a
truncated one only `nextSince` (or the automatic default) is correct.

A practical default for an agent: check `summary` first to see whether the
question is about a whole region (aggregate) or a specific node (filter), and
only then pull events — the page continues by itself if the first page
truncated.

## Step 4 — a worked session

Two small exercises show both halves of the loop: reading an effect off the
trace, and taking a deterministic snapshot.

### 4a. Find the throwing node (observation)

Run the demo with a seeded fault:

```bash
_build/<preset>/tools/web-debugger/debugger_demo --throw-every 20
```

Ask the agent:

> Attach to the debugger. Watch the trace until a node throws. Tell me which
> node, at which epoch, and what value it saw.

The agent's loop is `events_watch` → scan for `kind: "node_tick_throw"` →
`graph_get` to map the `nodeId` to its label. The event says, for example,
`{"kind":"node_tick_throw","clock":"ui","nodeId":5,"epoch":140,…}` and the
graph says node 5 is the `lift` with label `gain`, whose input arrives from the
`gain` boundary port. That is a diagnosis the agent reached from the live
program, not from reading the recipe.

Historical detail is available too: `events_tail` keeps returning the retained
tail, so the agent can ask "what happened in the ten events before the throw?"
by taking the event's sequence as a `since` cursor and re-reading.

### 4b. Write an input and verify the effect (injection + verification)

Now ask for a change:

> Set the `bpm` boundary input to 123 and prove from the trace that the change
> took effect.

The agent reads a cursor first, then writes:

```
events_tail {"nodeId": [0, 2]}       → { "nextSince": { "audio": 8123, "ui": 402 }, … }
input_set {"port":"bpm","value":123} → { "appliedEpoch": 12, "requestId": 41 }
events_tail {"since": { "audio": 8123, "ui": 402 }, "nodeId": [0, 2]} → the delta
```

Filtering to the `bpm` input node (0) and the `tempo` output node (2) keeps the
delta tiny; page with `nextSince` if a result reports `truncated: true`.

The demo computes `tempo = bpm * 2.0` on the audio clock via a `latest`
crossing, so the delta contains exactly the expected evidence:

```
traced effect: ui    node 0 = 123 (bpm)     at epoch 12
traced effect: audio node 2 = 246 (2 * 123) at epoch 98
```

Notice what makes this a *proof* and not a guess: the `bpm` input node's traced
value is 123 (it was overwritten to something else on the previous tick), and
the `tempo` output node's value is 246. The `appliedEpoch` tells the agent
which tick consumed the write; the events say what the write did.

**One warning about snapshots.** `output_get("tempo")` is a snapshot of the
last completed tick, and `debugger_demo`'s scripted driver keeps writing `bpm`
every 33 ms — so a `getOutput("tempo")` a moment later will *not* be 246, and
that is correct behaviour, not a bug. On a host whose driver does not overwrite
the input, snapshots are stable.

Rule of thumb: use the **traced effect** to prove a write landed on a moving
program; use `input_get`/`output_get` on a host that does not fight you for the
port when you want a deterministic read-back.

### 4c. Switch examples

`example_list` reports the examples the host can switch between, and
`example_select` flips the live one. Switching broadcasts a new graph to every
client, so afterwards the agent must re-read `graph_get` — the old port names
belong to the old example. The graph **resource** (`graph://<example>`, below)
moves with the switch too.

## Step 5 — the graph as a resource

Tools are verbs; the graph is also a noun. The server publishes the live graph
as the MCP resource `graph://<active example>`:

- `resources/list` → `[{ "uri": "graph://demo", "mimeType": "application/json", … }]`
- `resources/read` with `{"uri":"graph://demo"}` → the same JSON as `graph_get`.

An MCP client can load that into the conversation as a document. Only the
**active** example is addressable (see the limits): after `example_select`, the
URI changes, and reading the old one is an MCP resource error with an explicit
message rather than a stale document.

## Step 6 — the honest limits

Read these before attributing a failure to the wrong layer.

- **Active example only.** Every tool and the graph resource address the live
  example. `example` is reserved in the wire protocol but not implemented;
  switch, then re-read the graph.
- **Scalar injection only.** `input_set` accepts `unit`, `bool`, `int64` and
  `double` (plus the JSON mapping above; remember a JSON number is a *double* —
  write `{"kind":"int","value":42}` for an int64). User structs and text are
  rejected with an explicit "unsupported value kind" ack, because injecting a
  struct needs a runtime constructor from its schema that only a typed host
  setter can provide. Reading struct/text ports is fine.
- **No state patching.** `delay`/`scan` internals are not writable, and there
  is no per-region stepping or breakpoints — those need state-snapshot and
  scheduler semantics that are deferred design work.
- **Capture is global.** `pause` stops the trace recorder, not the program, and
  it gates every clock at once.
- **The trace is bounded.** The lock-free recorder drops under load and counts
  it (`dropped`); cursors older than the retained window cannot be replayed
  exactly. A busy audio region can be a *sample* of ticks, not every tick.
  Separately, every event query is capped at the protocol level
  (`maxEvents`/`maxBytes`) and pages with `nextSince` — see
  [Bounding what you ask for](#bounding-what-you-ask-for).
- **The write is queued.** A region with no ticker never runs the step hook's
  drain, so `input_set` times out rather than applying. A timeout means "the
  host may still apply it later" (the client's error says so); a drop means "it
  was lost and will never be acked".

## Troubleshooting

| Symptom | Likely cause | What to do |
| --- | --- | --- |
| Tool errors `not_connected` / `connect` | the debugger host is not running, or `--url` is wrong | start the host; check the port; the MCP server retries in the background |
| `input_set` error with code `ack` | unknown port (or wrong-kind port, or unsupported value kind) | `graph_get` for the exact input names; structs/text cannot be injected |
| `input_set` / `input_get` times out | the target region is not ticking (no step hook runs) | make sure the host ticks that clock; the write may still land later |
| `events_watch` returns `timedOut: true` | nothing recorded in the window — paused, or the region is quiet | `resume`, or use `events_tail` to check history and `dropped` |
| `events_tail` is empty right after connecting | the host has not recorded yet, or capture is paused | wait a tick, `resume` |
| `resources/read` gives `-32002` | you asked for a non-active example, or a non-`graph://` URI | `example_select` it first, then re-list; the URI is `graph://<active>` |
| The read-back is not the value just written | a scripted driver overwrites that input every tick | prove the write by its traced effect; use a stable port for snapshots |
| Agent sees old port names after a switch | tool results are snapshots; the graph changed | call `graph_get` again after `example_select` |
| `debugger_demo` exits on startup | the port is taken (the server binds exclusively) | pick another `--port` and match `--url` |

## Verifying the integration

The two test suites are the executable version of this page, and they need no
test framework or install:

```bash
cd tools/debug-mcp

# the MCP layer alone, against a mock client (no C++, no host)
node test/protocol.test.ts        # PASS: 220 ok, 0 failed

# the real loop: spawn debugger_demo, attach, observe, inject, verify, shut down
node test/e2e.test.ts             # PASS: 108 ok, 0 failed
```

The protocol suite also covers the bounded-event contract (caps, trim
direction, filters, summary counts, `nextSince` paging) against a deterministic
mock, including a deliberately large verbose trace. The e2e picks a free port
in 19000–19999, waits for the host's socket to accept connections, kills the
demo afterwards, and asserts every event-query result stays within its
`maxBytes` while paging with `nextSince`, and exercises the no-since
auto-paging continuation. Point it at
another binary with
`DEBUGGER_DEMO=/path/to/debugger_demo`. It is also a useful script to read if
you are writing a *new* MCP client — it shows the exact call/response shapes
this page describes.

## What is not here yet — and what is only a preview

- **Published package.** The server source ships in this repository; the npm
  publish (`npx clockr-debug-mcp`) is deferred. Until then use the local
  `node tools/debug-mcp/src/main.ts` form above. The relative client import
  (and node's refusal to type-strip inside `node_modules`) is why the published
  form needs a real packaged client dependency.
- **DeepSeek Harness plugin — preview only.** The same
  client is wrapped as a dsh/Cordis plugin exposing one service
  (`ctx.clockrDebug`) in
  [`tools/debug-dsh-plugin/`](../../tools/debug-dsh-plugin/README.md):
  source only, npm publish deferred, and **not booted in a real dsh process**
  (dsh itself is developer-preview). It is a thin lifecycle adapter — the
  protocol coupling stays in the client — and its README carries the
  compatibility-risk note. For agent-driven debugging today, the MCP server
  documented on this page is the shipped surface.
- **Per-example addressing / state patching / per-region stepping.** Deferred:
  they change the host contract or the semantics, and each needs its own
  design pass.

## Reference

| What | Where |
| --- | --- |
| the MCP server: tools, protocol subset, compatibility, tests | [`tools/debug-mcp/README.md`](../../tools/debug-mcp/README.md) |
| the debugger: wire schema, host contract, web UI | [`tools/web-debugger/README.md`](../../tools/web-debugger/README.md) |
| the TypeScript client the server wraps | [`tools/debug-client/src/index.ts`](../../tools/debug-client/src/index.ts) |
| the client package: API, codec sync policy, tests | [`tools/debug-client/README.md`](../../tools/debug-client/README.md) |
| why tick-boundary injection is the semantically correct boundary | [`../reference/denotational-semantics.md`](../reference/denotational-semantics.md) (laws L1/L2) |
| watching the same stream as a human | `debugger_demo` + `http://127.0.0.1:9002` |
