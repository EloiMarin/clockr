// Plugin test: exercise the Cordis adapter with a fake Cordis-like context and
// a fake debug client — no dsh runtime, no C++, no socket.
//
// What is pinned here:
//   - the `Config` export really is a Standard Schema v1 validator that Cordis's
//     `resolveConfig` can call (`Config['~standard'].validate`), with defaults
//     and issues;
//   - `apply(ctx, config)` registers EXACTLY ONE service (`ctx.clockrDebug`)
//     and one lifecycle effect, and unload closes the client;
//   - auto-connect happens on load, its failure is structured state
//     (`status().lastError`) rather than an unhandled rejection, and unload is
//     safe even when never connected;
//   - the service forwards every observation/injection method to the client
//     with the caller's arguments, and bigints leave through `status()` as
//     strings;
//   - the adapter source imports no `@deepseek-ai/*` package (the "keep the
//     contract structural, stay zero-dep" rule).
//
// Run: node test/plugin.test.ts

import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import {
  apply,
  Config,
  DEFAULT_URL,
  defaultConfig,
  PLUGIN_NAME,
  ClockrDebugService,
  SERVICE_NAME,
} from "../src/index.ts";

import { FakeClient } from "./support/fake-client.ts";
import { FakeContext } from "./support/fake-context.ts";
import { check, finish, ok, rejects, section, sleep } from "./support/harness.ts";

import type { EventBatchView } from "../../debug-client/src/index.ts";

const HERE = dirname(fileURLToPath(import.meta.url));
const SOURCE = resolve(HERE, "../src/index.ts");

// --- config schema ----------------------------------------------------------

section("Config: a Standard Schema validator Cordis can consume");

check("validate() fills the defaults", Config["~standard"].validate(undefined), {
  value: { url: DEFAULT_URL, connect: true },
});
check("validate({}) fills the defaults", Config["~standard"].validate({}), { value: defaultConfig() });
check("validate() accepts explicit url + connect", Config["~standard"].validate({ url: "ws://10.0.0.2:9100/ws", connect: false }), {
  value: { url: "ws://10.0.0.2:9100/ws", connect: false },
});
check("defaultConfig() is the documented default", defaultConfig(), { url: "ws://127.0.0.1:9002/ws", connect: true });

const badUrl = Config["~standard"].validate({ url: 42 }) as { issues?: Array<{ path?: unknown }> };
const badConnect = Config["~standard"].validate({ connect: "yes" }) as { issues?: Array<{ path?: unknown }> };
const badShape = Config["~standard"].validate("nope") as { issues?: unknown[] };

ok("validate() rejects a non-string url", Array.isArray(badUrl.issues) && badUrl.issues.length === 1, badUrl);
check("url issue is path-scoped", badUrl.issues?.[0]?.path, ["url"]);
ok(
  "validate() rejects a non-boolean connect",
  Array.isArray(badConnect.issues) && badConnect.issues.length === 1,
  badConnect,
);
check("connect issue is path-scoped", badConnect.issues?.[0]?.path, ["connect"]);
ok("validate() rejects a non-object config", Array.isArray(badShape.issues) && badShape.issues.length === 1, badShape);
ok("schema declares Standard Schema version 1", Config["~standard"].version === 1);
ok("schema declares a vendor", typeof Config["~standard"].vendor === "string" && Config["~standard"].vendor.length > 0);

// --- plugin identity --------------------------------------------------------

section("plugin identity and dependency-free source");

check("PLUGIN_NAME labels the plugin", PLUGIN_NAME, "clockr-debug");
check("SERVICE_NAME is the documented service", SERVICE_NAME, "clockrDebug");
ok("apply is a function (Cordis function-plugin shape)", typeof apply === "function");

const source = readFileSync(SOURCE, "utf8");

const harnessImports = source
  .split("\n")
  .filter((line) => /(?:from|import)\s*\(?\s*["']@deepseek-ai\//.test(line));

ok("the adapter imports no @deepseek-ai/* package", harnessImports.length === 0, harnessImports);
ok(
  "the adapter imports the debug client (the one runtime coupling)",
  source.includes('"../../debug-client/src/index.ts"'),
);

// --- apply: registration ----------------------------------------------------

section("apply: exactly one service and one lifecycle effect");

const context = new FakeContext();
const client = new FakeClient();
const service = (() => {
  apply(context, { url: "ws://fake.invalid/ws", connect: false }, { clientFactory: () => client });

  return context.get(SERVICE_NAME);
})();

ok("one service was provided", context.provided.length === 1, context.provided);
check("the service is named clockrDebug", context.provided[0]?.name, SERVICE_NAME);
ok("the service is the wrapper instance", service instanceof ClockrDebugService);

const wrapper = service as ClockrDebugService;

check("one lifecycle effect was registered", context.effectLabels.length, 1);
check("plugin config is exposed on the service", wrapper.url, "ws://fake.invalid/ws");
check("connect:false did not connect", client.connectCalls, 0);
check("the service starts idle", wrapper.state, "idle");
check("the service starts with no graph", wrapper.graph(), null);
check("status starts structured and closed:false", wrapper.status(), {
  url: "ws://fake.invalid/ws",
  closed: false,
  state: "idle",
  example: "demo",
  graph: null,
  cursors: { audio: "12", ui: "4" },
  dropped: { events: { ui: "3" }, commands: "1" },
  diagnostics: { decodeErrors: 0, orphanAcks: 0, pendingRequests: 0 },
  lastError: null,
});

// --- apply: auto-connect + unload ------------------------------------------

section("apply: auto-connect, then unload closes the client");

const context2 = new FakeContext();
const client2 = new FakeClient();

apply(context2, { url: "ws://fake.invalid/ws", connect: true }, { clientFactory: () => client2 });

await sleep(30);

const wrapper2 = context2.get(SERVICE_NAME) as ClockrDebugService;

check("auto-connect called the client once", client2.connectCalls, 1);
ok("auto-connect opened the connection", wrapper2.connected);
check("auto-connect status state", wrapper2.status().state, "open");
check("auto-connect status graph counts", wrapper2.status().graph, {
  nodes: 2,
  crossings: 1,
  inputs: 1,
  outputs: 1,
  types: 0,
});

await context2.disposeAll();

check("unload closed the client", client2.closeCalls, 1);
check("unload marked the service closed", wrapper2.closed, true);
check("unload removed the provided service", context2.services.size, 0);
await context2.disposeAll(); // idempotent
ok("disposing twice does not throw", true);

// --- apply: auto-connect failure is structured ------------------------------

section("apply: auto-connect failure is state, not an unhandled rejection");

const context3 = new FakeContext();
const failingClient = new FakeClient();
const failure = Object.assign(new Error("connection refused"), { code: "connect" });

failingClient.connectError = failure;

apply(context3, { url: "ws://fake.invalid/ws", connect: true }, { clientFactory: () => failingClient });

await sleep(40);

const wrapper3 = context3.get(SERVICE_NAME) as ClockrDebugService;

check("a failed auto-connect is recorded in status().lastError", wrapper3.status().lastError, {
  message: "connection refused",
  code: "connect",
});
check("a failed auto-connect leaves the client reconnecting", wrapper3.state, "reconnecting");
ok("a failed auto-connect did not crash the load", wrapper3.closed === false);

await context3.disposeAll();

check("unload after a failed connect still closes the client", failingClient.closeCalls, 1);
ok("unload after a failed connect does not throw", true);

// --- apply: dispose when never connected -----------------------------------

section("apply: dispose before ever connecting is safe");

const context4 = new FakeContext();
const client4 = new FakeClient();

apply(context4, { url: "ws://fake.invalid/ws", connect: false }, { clientFactory: () => client4 });
await context4.disposeAll();
await context4.disposeAll();

const wrapper4 = context4.get(SERVICE_NAME);

check("never-connected dispose closed the client", client4.closeCalls, 1);
check("never-connected dispose removed the service", wrapper4, undefined);
ok("never-connected dispose is idempotent", true);

// --- service: forwarding ----------------------------------------------------

section("service: forwards observation and injection to the client");

const fake = new FakeClient();
const direct = new ClockrDebugService({
  url: "ws://fake.invalid/ws",
  connect: false,
  clientFactory: () => fake,
});

const first = direct.connect();
const second = direct.connect();
const [graph1, graph2] = await Promise.all([first, second]);

check("concurrent connect() shares one attempt", fake.connectCalls, 1);
check("connect() resolves the graph", graph1.name, "demo");
ok("both connect() callers get the graph", graph1 === graph2);
check("status reflects the connection", direct.status().state, "open");
check("status reflects the example", direct.status().example, "demo");

const batches: EventBatchView[] = [];

for await (const batch of direct.events({ live: false })) batches.push(batch);

check("events() forwards the stream", batches.map((batch) => batch.clock), ["audio", "ui"]);
check(
  "events() forwards the events",
  batches.flatMap((batch) => batch.events).length,
  3,
);

const tail = await direct.tailEvents(2);

check("tailEvents(limit) keeps the last events", tail.map((event) => event.sequence), [2n, 1n]);
check(
  "tailEvents(since) filters per clock",
  (await direct.tailEvents(1, { audio: 1n, ui: 1n })).map((event) => event.sequence),
  [2n],
);

const injection = await direct.setInput("bpm", 123, { timeoutMs: 1_234 });

check("setInput forwards the ack", injection, { requestId: 7n, appliedEpoch: 11n });
check("setInput forwards port/value/options", fake.lastSetInput, { port: "bpm", value: 123, options: { timeoutMs: 1_234 } });

check("getInput forwards", (await direct.getInput("bpm")).value, { kind: "double", value: 120 });
check("getOutput forwards", (await direct.getOutput("tempo")).value, { kind: "double", value: 246 });
check("examples forwards", await direct.examples(), { names: ["demo", "daw"], active: "demo" });

await direct.selectExample("daw");

check("selectExample forwards and updates the example", direct.currentExample, "daw");
check("selectExample changed the graph", direct.graph()?.name, "daw");

await direct.pause();
await direct.resume();

check("pause forwarded", fake.pauseCalls, 1);
check("resume forwarded", fake.resumeCalls, 1);
ok("the recording client saw the calls", fake.calls.includes("getInput:bpm") && fake.calls.includes("selectExample:daw"));

// --- service: lifecycle -----------------------------------------------------

section("service: close is idempotent and a closed service refuses connect");

direct.close();
direct.close();

check("close forwarded once", fake.closeCalls, 1);
check("closed status", direct.status().closed, true);
check("closed state", direct.state, "closed");

const closedError = await rejects("connect() rejects after close", direct.connect());

ok("close error is explicit", closedError instanceof Error && closedError.message.includes("closed"), closedError);

// --- service: lastError recovery -------------------------------------------

section("service: a later successful connect clears lastError");

const flaky = new FakeClient();
const flakyService = new ClockrDebugService({
  url: "ws://fake.invalid/ws",
  connect: false,
  clientFactory: () => flaky,
});

flaky.connectError = Object.assign(new Error("ECONNREFUSED"), { code: "connect" });
await rejects("first connect rejects", flakyService.connect());

check("failure is recorded", flakyService.status().lastError, { message: "ECONNREFUSED", code: "connect" });

flaky.connectError = null;
await flakyService.connect();

check("success clears lastError", flakyService.status().lastError, null);

flakyService.close();
check("closed after cleanup", flakyService.closed, true);

finish();
