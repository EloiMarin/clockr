# clockr-debug-dsh-plugin — DeepSeek Harness (Cordis) adapter

> ## ⚠️ Developer preview — compatibility risk
>
> **DeepSeek Harness (`dsh`) is developer-preview, and the Cordis plugin
> contract this package targets may change.** This package is a **scaffold**
> shipped as source: it is not published to npm, it is not part of dsh's
> built-in bundle, and it has **not** been booted inside a real dsh process.
>
> What *is* verified: the plugin shape (`name` / `Config` / `apply`), the
> Standard Schema config validation Cordis performs, and the
> `ctx.provide` / `ctx.effect` lifecycle it relies on — against the read-only
> dsh checkout's vendored Cordis implementation
> (`vendor/cordis/src/{fiber,reflect}.ts`) and its tutorials, plus a fake-context
> test (below). What is *preview-risky* is listed explicitly in
> [Verified vs preview-risky](#verified-vs-preview-risky). Do not treat this
> package as a stable dsh integration.
>
> The safety valve is the design: **all wire-protocol coupling lives in
> `clockr-debug-client`**, which is independently tested against the real C++
> debugger (see [`tools/debug-mcp/README.md`](../debug-mcp/README.md) for the real
> end-to-end run). This package only adapts lifecycle and service shape, so a
> Cordis API change breaks this thin adapter, not the debugging capability.

This package wraps the same TypeScript debug client as **one Cordis service**
(`ctx.clockrDebug`) so a dsh plugin (or the harness itself) can attach to a
live `clockr` host, watch the trace, and write inputs back at tick boundaries.

```
host tick loop ──▶ debugger server ──▶ WebSocket ──▶ clockr-debug-client
                                                              │
                                                   clockr-debug-dsh-plugin
                                                              │
                                                   Cordis service: ctx.clockrDebug
                                                              │
                                                dsh plugins / tools / the harness
```

The human-facing workflow is
[`doc/how-to/agent-live-debug.md`](../../../doc/how-to/agent-live-debug.md);
the MCP server (for non-dsh agents) is
[`tools/debug-mcp/`](../debug-mcp/README.md). This package is for dsh-native
consumers.

## What it exposes

`apply(ctx, config)` registers exactly one service, `ctx.clockrDebug`, and
one lifecycle effect (close the socket on unload). The service is a thin,
lifecycle-aware wrapper over the debug client:

| Member | Signature | Behaviour |
| --- | --- | --- |
| `client` | `DebugClientLike` | escape hatch to the wrapped client |
| `state` | `"idle" \| "connecting" \| "open" \| "reconnecting" \| "closed"` | forwarded connection state |
| `connected` | `boolean` | `true` while the socket is open |
| `closed` | `boolean` | `true` after `close()` / plugin unload |
| `currentExample` | `string` | the live example's name |
| `lastError` | `{ message, code? } \| null` | last connect failure, cleared on success |
| `status()` | `DebugStatus` | structured JSON-friendly snapshot (see below) |
| `connect()` | `Promise<GraphDescription>` | deduplicated in-flight connect; resolves the graph |
| `close()` | `void` | idempotent; stops reconnecting |
| `graph()` | `GraphDescription \| null` | the last graph description |
| `events(options?)` | `AsyncGenerator<EventBatchView>` | the trace stream (history replay + live) |
| `tailEvents(limit?, since?)` | `Promise<TraceEvent[]>` | retained tail convenience (last `limit`, per-clock `since`) |
| `examples()` | `Promise<ExampleList>` | example names + active |
| `selectExample(name)` | `Promise<void>` | switch the live example |
| `setInput(port, value, options?)` | `Promise<{requestId, appliedEpoch}>` | tick-boundary injection |
| `getInput(port, options?)` / `getOutput(port, options?)` | `Promise<PortValue>` | boundary snapshot read-back |
| `pause()` / `resume()` | `Promise<void>` | trace-capture gate |

`status()` is the structured state an operator tool wants without reaching
into the client:

```ts
{
  url, closed, state, example,
  graph: { nodes, crossings, inputs, outputs, types } | null,
  cursors: { [clock]: string },              // bigint as decimal strings
  dropped: { events: { [clock]: string }, commands: string },
  diagnostics: { decodeErrors, orphanAcks, pendingRequests },
  lastError: { message, code? } | null,
}
```

Bigints never leak into JSON here: `cursors` and `dropped` are decimal
strings, because the same `status()` object is meant to be logged or shown in
a UI.

## Configuration

```yaml
# cordis.yml (or a deployment overlay / patch)
- name: './tools/debug-dsh-plugin/src/index.ts'
  config:
    url: ws://127.0.0.1:9002/ws   # default
    connect: true                 # default; connect eagerly on load
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `url` | `string` | `ws://127.0.0.1:9002/ws` | the debugger host's WebSocket endpoint |
| `connect` | `boolean` | `true` | auto-connect on load; a failure is recorded in `status().lastError` and the client keeps retrying in the background |

Unknown keys are ignored (the harness loader may add metadata; this scaffold
does not reject a composition over an extra key it does not read). Invalid
values fail the load with a path-scoped validation error, as Cordis does for
any plugin.

The package is mounted **by path** today because it is not published. Its
client import is relative (`../../debug-client/src/index.ts`), so it must live at
`tools/debug-dsh-plugin/` inside this repository, and dsh's loader must be
able to run TypeScript (the harness runs its own sources through tsx). There
is no build step.

### Consuming the service from another dsh plugin

```ts
import type { Context } from '@deepseek-ai/cordis'

export const name = 'my-debug-tool'
export const inject = ['clockrDebug']

export function apply(ctx: Context) {
  const debug = ctx.clockrDebug

  debug.events({ live: true })
  // ... or use debug.status(), debug.graph(), debug.setInput(...)
}
```

`inject` is a hard requirement in Cordis: a consumer listed this way stays
PENDING until this plugin is loaded. For optional use, call
`ctx.get('clockrDebug')` and handle `undefined` instead.

TypeScript consumers can recover the typed `ctx.clockrDebug` property with
declaration merging (this package cannot declare it itself without taking a
dependency on `@deepseek-ai/cordis`):

```ts
import type { ClockrDebugService } from './tools/debug-dsh-plugin/src/index.ts'

declare module '@deepseek-ai/cordis' {
  interface Context {
    clockrDebug: ClockrDebugService
  }
}
```

## Run a host and try it

```bash
# 1) the debugger host (the demo is easiest; any host embedding debug::server works)
./build.sh
_build/<preset>/tools/web-debugger/debugger_demo        # ws://127.0.0.1:9002/ws

# 2) mount the plugin in a dsh composition, then ask `status()` / `events()` /
#    `setInput()` from whatever consumes the service. The wire semantics are
#    identical to the MCP tools; see doc/how-to/agent-live-debug.md.
```

If the host is not running, mounting still succeeds: auto-connect fails, the
failure is available as `ctx.clockrDebug.status().lastError`, and the client
retries in the background (start order does not matter).

## Tests

Plain node, no framework, no install:

```bash
cd tools/debug-dsh-plugin
node test/plugin.test.ts       # PASS: 69 ok, 0 failed
```

The test uses a **fake Cordis-like context** and a **fake debug client**
(`test/support/`) — no dsh boot, no socket, no C++ — and pins:

- `Config` is a real Standard Schema v1 validator (`Config['~standard'].validate`),
  with defaults and path-scoped issues for bad `url`/`connect`;
- `apply(ctx, config)` provides **exactly one** service named `clockrDebug`
  and registers one lifecycle effect;
- unload closes the client and unregisters the service; disposing twice, or
  disposing before ever connecting, never throws;
- auto-connect happens on load, a failed auto-connect becomes structured state
  (`status().lastError`) instead of an unhandled rejection, and a later
  successful connect clears it;
- the service forwards `connect`/`events`/`tailEvents`/`setInput`/`getInput`/
  `getOutput`/`examples`/`selectExample`/`pause`/`resume` with the caller's
  arguments, and `status()` is bigint-safe;
- the adapter source imports no `@deepseek-ai/*` package (zero runtime
  dependencies; the contract is structural).

The wire path itself is covered elsewhere and for real:
`tools/debug-client/test/*.test.ts` and `tools/debug-mcp/test/e2e.test.ts`
(the latter spawns `debugger_demo` and proves injection → traced effect).

## Verified vs preview-risky

**Verified** (against the read-only dsh checkout and the test above):

- a function plugin module exports `name`, `Config`, `apply(ctx, config)`, and
  Cordis calls `apply` after `Config['~standard'].validate(raw)` succeeds
  (`vendor/cordis/src/fiber.ts` — `resolveConfig`);
- `ctx.provide(name, value)` registers a service owned by the plugin
  fiber and returns an effect disposer; `ctx.effect(body)` keeps the disposer
  returned by `body` and runs it on unload (`vendor/cordis/src/reflect.ts`,
  `vendor/cordis/src/fiber.ts`);
- a plain object exported as `Config` would **not** work (Cordis requires a
  Standard Schema validator), which is why the schema is hand-rolled;
- the adapter's runtime behaviour with a fake context/client (test above).

**Preview-risky** (documented, not hidden):

- the preview loader's module resolution rules (path vs package specifier, TS
  execution) may change; mounting by path is a scaffold, not a publish story;
- service naming is application-global: another plugin claiming
  `clockrDebug` would collide (unload order then matters);
- Standard Schema adoption could tighten (e.g. require the optional `types`
  field, or async validation); the adapter is sync-schema only;
- no real dsh boot is exercised here; the loader may add expectations beyond
  the module contract (lifecycle timing, HMR/reload behaviour, logger usage)
  that this scaffold does not cover;
- `connect: true` as the default is a judgement call for a developer tool; an
  embedding application may prefer `connect: false` and an explicit
  `ctx.clockrDebug.connect()`.

**Not attempted** (deliberately, to avoid faking a deep integration): dsh UI/
tools registration, LLM-facing tool schemas (that is the MCP package's job),
session/agent integration, settings cards, persistence, or any dsh service
beyond `ctx.provide`/`ctx.effect`.

## Limitations

Inherited from the client and the debugger protocol (details in
[`tools/debug-mcp/README.md`](../debug-mcp/README.md)): active example only
(per-example addressing deferred), scalar injection only
(struct/text write-back deferred), no `delay`/`scan` state patching, no
per-region stepping/breakpoints, globally gated capture, and a bounded trace
that counts drops rather than blocking.

## Layout

```
dsh-plugin/
├── src/index.ts            plugin: name, Config (Standard Schema), service, apply
├── test/
│   ├── plugin.test.ts      fake-context/fake-client contract test
│   └── support/            harness, FakeContext, FakeClient
├── package.json            private, zero dependencies, test scripts
├── tsconfig.json           editor/type-check only; no build step
└── README.md               this file (preview warning lives at the top)
```
