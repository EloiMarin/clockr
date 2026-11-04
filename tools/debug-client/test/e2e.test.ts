// End-to-end harness: spawn the real `debugger_demo` (the debug server), drive
// the client against it, and assert the whole debug-agent loop:
//
//   graph + examples received, trace events flow, a `setInput` reaches a tick
//   boundary (Ack with an applied epoch) and shows up in the traced stream, a
//   read-back round-trips, pause/resume gates capture, and drop accounting is
//   surfaced (never silent).
//
// This is a plain node process: no test framework, no browser, no npm install.
//
//   node test/e2e.test.ts
//   DEBUGGER_DEMO=/path/to/debugger_demo node test/e2e.test.ts
//
// The binary defaults to _build/linux_x64_debug/tools/web-debugger/debugger_demo;
// the demo runs on a free port in 19000-19999 and is
// killed afterwards. If the binary does not exist the harness prints SKIP and
// exits 0, so the unit-test suite can run without a C++ build.

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync } from "node:fs";
import { createServer, connect } from "node:net";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { createClient, type DebugClient, type EventBatchView, type GraphDescription, type TraceEvent } from "../src/index.ts";

import { check, finish, ok, section, sleep, waitFor } from "./support/harness.ts";

// --- configuration -----------------------------------------------------------

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, "../../..");
const DEMO =
  process.env.DEBUGGER_DEMO ?? resolve(REPO_ROOT, "_build/linux_x64_debug/tools/web-debugger/debugger_demo");
const WEB_ROOT = resolve(REPO_ROOT, "tools/web-debugger/web");

if (!existsSync(DEMO)) {
  console.log(`SKIP e2e: debugger_demo not found at ${DEMO}`);
  console.log("      build a preset, or set DEBUGGER_DEMO=/path/to/debugger_demo");
  process.exit(0);
}

const STARTUP_TIMEOUT_MS = 30_000;
const REQUEST_TIMEOUT_MS = 3_000;
const VERBOSE = process.env.E2E_DEBUG === "1";

const verbose = (message: string): void => {
  if (VERBOSE) console.log(`[e2e] ${message}`);
};

// --- process / port plumbing -------------------------------------------------

function isPortFree(port: number): Promise<boolean> {
  return new Promise((resolveFree) => {
    const server = createServer();

    server.once("error", () => resolveFree(false));
    server.listen(port, "127.0.0.1", () => server.close(() => resolveFree(true)));
  });
}

async function pickPort(): Promise<number> {
  for (let attempt = 0; attempt < 20; attempt += 1) {
    const port = 19_000 + Math.floor(Math.random() * 1_000);

    if (await isPortFree(port)) return port;
  }

  throw new Error("e2e: could not find a free port in 19000-19999");
}

function tcpReachable(port: number): Promise<boolean> {
  return new Promise((resolveReachable) => {
    const socket = connect({ host: "127.0.0.1", port });

    const settle = (reachable: boolean): void => {
      socket.removeAllListeners();
      socket.destroy();
      resolveReachable(reachable);
    };

    socket.setTimeout(250, () => settle(false));
    socket.once("connect", () => settle(true));
    socket.once("error", () => settle(false));
  });
}

interface DemoProcess {
  child: ChildProcessWithoutNullStreams;
  port: number;
  output: () => string;
}

/**
 * Spawn debugger_demo on a free port and wait until it listens. stdout is
 * block-buffered when piped (the demo writes "\n", not std::endl), so the
 * "listening on" line can lag arbitrarily; polling the TCP port is the
 * reliable readiness signal.
 */
async function startDemo(): Promise<DemoProcess> {
  const port = await pickPort();
  const args = ["--host", "127.0.0.1", "--port", String(port), "--web-root", WEB_ROOT];

  verbose(`spawning ${DEMO} ${args.join(" ")}`);
  console.log(`spawn: ${DEMO} ${args.join(" ")}`);

  const child = spawn(DEMO, args, { cwd: REPO_ROOT, stdio: ["ignore", "pipe", "pipe"] }) as ChildProcessWithoutNullStreams;

  let childOutput = "";

  child.stdout.on("data", (chunk: Buffer) => {
    childOutput += chunk.toString();
    verbose(`demo stdout: ${chunk.toString().trim()}`);
  });
  child.stderr.on("data", (chunk: Buffer) => {
    childOutput += chunk.toString();
    verbose(`demo stderr: ${chunk.toString().trim()}`);
  });

  let exitNote = "";

  child.once("exit", (code, signal) => {
    exitNote = `\n[debugger_demo exited: code=${String(code)} signal=${String(signal)}]`;
  });

  const deadline = Date.now() + STARTUP_TIMEOUT_MS;

  while (Date.now() < deadline) {
    if (child.exitCode !== null) {
      throw new Error(`debugger_demo exited before listening (code ${child.exitCode})${exitNote}\n${childOutput}`);
    }

    if (await tcpReachable(port)) {
      console.log(`ready: 127.0.0.1:${port} accepting connections`);

      return { child, port, output: () => childOutput + exitNote };
    }

    await sleep(50);
  }

  child.kill("SIGKILL");
  throw new Error(`debugger_demo did not listen within ${STARTUP_TIMEOUT_MS} ms${exitNote}\n${childOutput}`);
}

function killDemo(demo: DemoProcess | null): void {
  if (demo === null) return;

  try {
    demo.child.kill("SIGKILL");
  } catch {
    // already gone
  }
}

// --- event collection --------------------------------------------------------

/** A live iterator draining `client.events()` into arrays the assertions poll. */
class EventCollector {
  readonly events: TraceEvent[] = [];
  readonly batches: EventBatchView[] = [];
  error: Error | null = null;

  #controller = new AbortController();
  #pump: Promise<void>;

  constructor(client: DebugClient) {
    this.#pump = this.#run(client);
  }

  async #run(client: DebugClient): Promise<void> {
    try {
      for await (const batch of client.events({ signal: this.#controller.signal })) {
        this.batches.push(batch);

        for (const event of batch.events) this.events.push(event);
      }
    } catch (error) {
      if ((error as Error).name !== "AbortError") this.error = error as Error;
    }
  }

  async stop(): Promise<void> {
    this.#controller.abort();
    await this.#pump;
  }

  eventsSince(index: number): TraceEvent[] {
    return this.events.slice(index);
  }

  get clocks(): string[] {
    return [...new Set(this.events.map((event) => event.clock))].sort();
  }

  /** Sum of the per-batch dropped counts the client reported, by clock. */
  droppedByClock(): Record<string, bigint> {
    const totals: Record<string, bigint> = {};

    for (const batch of this.batches) {
      if (batch.dropped === 0n) continue;
      totals[batch.clock] = (totals[batch.clock] ?? 0n) + batch.dropped;
    }

    return totals;
  }
}

function portNames(ports: Array<{ name: string }>): string[] {
  return ports.map((port) => port.name);
}

function findNodeId(graph: GraphDescription, ports: Array<{ name: string; nodeId: bigint }>, name: string): bigint | null {
  const port = ports.find((entry) => entry.name === name);

  return port === undefined ? null : port.nodeId;
}

/** True when, per clock, sequence numbers strictly increase in arrival order. */
function monotonicPerClock(events: TraceEvent[]): boolean {
  const perClock = new Map<string, bigint[]>();

  for (const event of events) {
    const sequences = perClock.get(event.clock) ?? [];

    sequences.push(event.sequence);
    perClock.set(event.clock, sequences);
  }

  let monotonic = true;

  for (const [clock, sequences] of perClock) {
    for (let index = 1; index < sequences.length; index += 1) {
      if (sequences[index]! <= sequences[index - 1]!) {
        const globalIndex = events.findIndex((event) => event.clock === clock && event.sequence === sequences[index]);
        const around = events.slice(Math.max(0, globalIndex - 2), globalIndex + 3).map((event) =>
          `${event.clock}#${String(event.sequence)},${event.kind},node=${String(event.nodeId)},ep=${String(event.epoch)}`);
        console.log(
          `non-monotonic ${clock}: ${String(sequences[index - 1])} -> ${String(sequences[index])} ` +
            `at index ${index} of ${sequences.length}; around: ${around.join(" | ")}`,
        );
        monotonic = false;
        break;
      }
    }
  }

  return monotonic;
}

// --- the run -----------------------------------------------------------------

let demo: DemoProcess | null = null;
let client: DebugClient | null = null;

try {
  demo = await startDemo();

  section("client: connect + graph + examples");

  client = createClient(`ws://127.0.0.1:${demo.port}/ws`, {
    requestTimeoutMs: REQUEST_TIMEOUT_MS,
    connectTimeoutMs: STARTUP_TIMEOUT_MS,
    debug: VERBOSE ? (message) => console.log(`[client] ${message}`) : undefined,
  });

  const graph = await client.connect();

  console.log(
    `graph: name="${graph.name}" nodes=${graph.nodes.length} inputs=[${portNames(graph.inputs).join(",")}] ` +
      `outputs=[${portNames(graph.outputs).join(",")}] types=${graph.types.length}`,
  );

  check("graph example is demo", graph.name, "demo");
  ok("graph has nodes", graph.nodes.length > 5, graph.nodes.length);
  ok("graph carries the demo inputs", ["bpm", "gain", "trigger", "loop"].every((name) => portNames(graph.inputs).includes(name)));
  ok(
    "graph carries the demo outputs",
    ["tempo", "ticks", "gain_out", "triggers", "loop_len"].every((name) => portNames(graph.outputs).includes(name)),
  );

  const examples = await client.examples();

  check("examples list", examples, { names: ["demo"], active: "demo" });

  const tempoNode = findNodeId(graph, graph.outputs, "tempo");
  const bpmNode = findNodeId(graph, graph.inputs, "bpm");

  ok("tempo output node resolved", tempoNode !== null);
  ok("bpm input node resolved", bpmNode !== null);

  section("client: live trace events");

  const collector = new EventCollector(client);

  await waitFor("trace events flow (>= 40)", () => collector.events.length >= 40, { timeoutMs: 10_000 });

  ok("events arrive from >= 2 clocks", collector.clocks.length >= 2, collector.clocks);
  ok("every event names a clock", collector.events.every((event) => event.clock !== ""));
  ok(
    "every event has a known kind",
    collector.events.every((event) =>
      ["region_step_begin", "region_step_end", "node_tick_begin", "node_tick_end", "node_tick_throw"].includes(
        event.kind,
      ),
    ),
  );
  ok(
    "sequence numbers are non-negative bigint",
    collector.events.every((event) => typeof event.sequence === "bigint" && event.sequence >= 0n),
    collector.events.map((event) => String(event.sequence)).slice(0, 8),
  );
  ok("trace carries node values", collector.events.some((event) => event.kind === "node_tick_end" && event.value !== null));

  ok("per-clock sequences strictly increase in arrival order (demo phase)", monotonicPerClock(collector.events));

  // Sequence counters are per engine.

  section("client: setInput -> Ack -> traced effect");

  const beforeInjection = collector.events.length;
  const ack = await client.setInput("bpm", 123.0);

  console.log(`ack: request_id=${String(ack.requestId)} applied_epoch=${String(ack.appliedEpoch)}`);
  ok("setInput ack request id is bigint", typeof ack.requestId === "bigint" && ack.requestId > 0n);
  ok("setInput ack applied_epoch > 0", typeof ack.appliedEpoch === "bigint" && ack.appliedEpoch > 0n);

  // The demo's audio chain computes tempo = bpm * 2 on the audio clock while
  // the injected value is live (the ui driver overwrites bpm on its own
  // cadence, so the value is only visible for a short epoch window — the
  // traced stream is the authoritative assertion here).
  const tempoEffect = (): TraceEvent | undefined =>
    collector.eventsSince(beforeInjection).find(
      (event) =>
        event.nodeId === tempoNode &&
        event.kind === "node_tick_end" &&
        event.value?.kind === "double" &&
        event.value.value === 246,
    );

  await waitFor("traced tempo node reaches 246 (2 * injected bpm)", () => tempoEffect() !== undefined, {
    timeoutMs: 10_000,
  });

  const effectEvent = tempoEffect();

  if (effectEvent !== undefined) {
    console.log(`traced effect: tempo node ${String(effectEvent.nodeId)} = 246.0 at epoch ${String(effectEvent.epoch)}`);
  }

  const bpmNodeEvent = (): TraceEvent | undefined =>
    collector.eventsSince(beforeInjection).find(
      (event) =>
        event.nodeId === bpmNode &&
        event.kind === "node_tick_end" &&
        event.value?.kind === "double" &&
        event.value.value === 123,
    );

  ok("the injected bpm value appears in the input node's trace", bpmNodeEvent() !== undefined);

  section("client: read-back (getOutput on demo)");

  const tempoRead = await client.getOutput("tempo");

  console.log(`getOutput("tempo"): port=${tempoRead.port} value=${JSON.stringify(tempoRead.value)}`);
  check("getOutput resolves the tempo port", tempoRead.port, "tempo");
  ok(
    "getOutput tempo is a double",
    tempoRead.value.kind === "double" && Number.isFinite(tempoRead.value.value),
    tempoRead.value,
  );

  section("client: pause / resume");

  await client.pause();
  await sleep(200); // let the drain timer flush anything captured before the gate closed
  const pausedBaseline = collector.events.length;

  await sleep(400);
  check("no trace events arrive while paused", collector.events.length, pausedBaseline);

  await client.resume();
  await waitFor("trace events resume after resume()", () => collector.events.length > pausedBaseline + 5, {
    timeoutMs: 5_000,
  });

  section("client: drop accounting");

  ok("no frame decode errors", client.decodeErrors === 0, client.decodeErrors);
  ok("no orphan acks", client.orphanAcks === 0, client.orphanAcks);

  const droppedTotals = collector.droppedByClock();
  const clientDropped = client.dropped;

  console.log(
    `dropped trace events by clock: ${JSON.stringify(
      Object.fromEntries(Object.entries(clientDropped.events).map(([clock, count]) => [clock, String(count)])),
    )}`,
  );
  console.log(`dropped commands: ${String(clientDropped.commands)}`);
  check("client dropped-event totals match the yielded batches", clientDropped.events, droppedTotals);
  ok("dropped counts are bigint", Object.values(clientDropped.events).every((count) => typeof count === "bigint"));
  check("no commands were dropped", clientDropped.commands, 0n);

  await collector.stop();
  client.close();
  check("client reports closed", client.state, "closed");

  console.log("\nE2E PASS");
  ok("e2e completed", true);
} catch (error) {
  console.log(`\nE2E FAIL: ${(error as Error).message}`);
  ok("e2e completed", false, (error as Error).stack);
  process.exitCode = 1;
} finally {
  client?.close();
  killDemo(demo);
}

finish("e2e.test.ts");
