// A fake `DebugClient` for the plugin test.
//
// The plugin is a thin adapter, so the thing to verify is *forwarding and
// lifecycle*, not the protocol (the debug client tests and the real MCP
// e2e cover the wire). This fake records every call and returns canned values,
// letting the test assert exact interactions without a WebSocket or a C++
// host — and therefore without a dsh runtime.

import type {
  ConnectionState,
  DropStats,
  EventBatchView,
  EventsOptions,
  ExampleList,
  GraphDescription,
  PortValue,
  RequestOptions,
  SetInputResult,
  TraceEvent,
  Value,
  ValueInput,
} from "../../../debug-client/src/index.ts";

import type { DebugClientLike } from "../../src/index.ts";

function node(id: bigint, kind: string, clock: string, label: string, port: string, fanIn: bigint[] = []) {
  return { id, kind, clock, label, port, fanIn };
}

export const FAKE_DEMO_GRAPH: GraphDescription = {
  name: "demo",
  nodes: [node(1n, "input", "ui", "input", "bpm"), node(2n, "output", "audio", "output", "tempo")],
  crossings: [{ from: 1n, to: 2n, policy: "latest" }],
  inputs: [{ nodeId: 1n, name: "bpm" }],
  outputs: [{ nodeId: 2n, name: "tempo" }],
  types: [],
};

export const FAKE_DAW_GRAPH: GraphDescription = {
  name: "daw",
  nodes: [node(11n, "input", "ui", "input", "playing"), node(12n, "output", "audio", "output", "playhead")],
  crossings: [{ from: 11n, to: 12n, policy: "latest" }],
  inputs: [{ nodeId: 11n, name: "playing" }],
  outputs: [{ nodeId: 12n, name: "playhead" }],
  types: [],
};

function event(sequence: bigint, clock: string, kind: string, nodeId: bigint, value: Value | null): TraceEvent {
  return {
    sequence,
    stampNs: sequence * 1_000_000n,
    kind: kind as TraceEvent["kind"],
    nodeId,
    clock,
    epoch: 3n,
    value,
    threw: false,
    durationNs: 2_000n,
  };
}

const FAKE_HISTORY: EventBatchView[] = [
  {
    clock: "audio",
    dropped: 0n,
    replay: true,
    events: [
      event(1n, "audio", "node_tick_end", 2n, { kind: "double", value: 240 }),
      event(2n, "audio", "node_tick_end", 2n, { kind: "double", value: 240 }),
    ],
  },
  {
    clock: "ui",
    dropped: 0n,
    replay: true,
    events: [event(1n, "ui", "node_tick_end", 1n, { kind: "double", value: 120 })],
  },
];

const sleep = (ms: number): Promise<void> => new Promise((resolveSleep) => setTimeout(resolveSleep, ms));

/** A recording, canned-answer fake of the debug client. */
export class FakeClient implements DebugClientLike {
  readonly url: string;
  state: ConnectionState = "idle";
  graph: GraphDescription | null = null;
  currentExample = "demo";
  cursors: Record<string, bigint> = { audio: 12n, ui: 4n };
  dropped: DropStats = { events: { ui: 3n }, commands: 1n };
  decodeErrors = 0;
  orphanAcks = 0;
  pendingRequestCount = 0;

  readonly calls: string[] = [];
  connectCalls = 0;
  closeCalls = 0;
  pauseCalls = 0;
  resumeCalls = 0;
  connectError: Error | null = null;
  lastSetInput: { port: string; value: ValueInput; options?: RequestOptions } | null = null;
  history: EventBatchView[] = [...FAKE_HISTORY];
  connectResult: GraphDescription = FAKE_DEMO_GRAPH;

  constructor(url = "ws://fake.invalid/ws") {
    this.url = url;
  }

  async connect(): Promise<GraphDescription> {
    this.connectCalls += 1;
    this.calls.push("connect");

    await sleep(5);

    if (this.connectError !== null) {
      this.state = "reconnecting";
      throw this.connectError;
    }

    this.state = "open";
    this.graph = this.connectResult;

    return this.connectResult;
  }

  close(): void {
    this.closeCalls += 1;
    this.state = "closed";
  }

  async *events(options: EventsOptions = {}): AsyncGenerator<EventBatchView, void, void> {
    this.calls.push("events");

    const since = new Map<string, bigint>();

    if (options.sinceSequence !== undefined) {
      for (const [clock, sequence] of Object.entries(options.sinceSequence)) since.set(clock, BigInt(sequence));
    }

    for (const batch of this.history) {
      const cursor = since.get(batch.clock);
      const events = cursor === undefined ? batch.events : batch.events.filter((entry) => entry.sequence > cursor);

      if (events.length === 0) continue;

      yield { clock: batch.clock, dropped: batch.dropped, events, replay: batch.replay };
    }
  }

  async setInput(port: string, value: ValueInput, options: RequestOptions = {}): Promise<SetInputResult> {
    this.calls.push(`setInput:${port}`);
    this.lastSetInput = { port, value, options };

    return { requestId: 7n, appliedEpoch: 11n };
  }

  async getInput(port: string, _options: RequestOptions = {}): Promise<PortValue> {
    this.calls.push(`getInput:${port}`);

    return { requestId: 8n, port, value: { kind: "double", value: 120 }, error: "" };
  }

  async getOutput(port: string, _options: RequestOptions = {}): Promise<PortValue> {
    this.calls.push(`getOutput:${port}`);

    return { requestId: 9n, port, value: { kind: "double", value: 246 }, error: "" };
  }

  async examples(): Promise<ExampleList> {
    this.calls.push("examples");

    return { names: ["demo", "daw"], active: this.currentExample };
  }

  async selectExample(name: string): Promise<void> {
    this.calls.push(`selectExample:${name}`);

    if (name !== "demo" && name !== "daw") throw new Error(`unknown example "${name}"`);

    this.currentExample = name;
    this.graph = name === "daw" ? FAKE_DAW_GRAPH : FAKE_DEMO_GRAPH;
  }

  async pause(): Promise<void> {
    this.pauseCalls += 1;
    this.calls.push("pause");
  }

  async resume(): Promise<void> {
    this.resumeCalls += 1;
    this.calls.push("resume");
  }
}
