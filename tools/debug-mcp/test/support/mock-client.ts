// A deterministic mock of the debug client's `DebugClient`, injected into the MCP
// server through main.ts's `--client-module` test seam.
//
// Why a mock at all, when the real e2e exists? The protocol test must pin the
// MCP layer — handshake, tools/list, JSON-RPC errors, argument validation,
// resource URIs, stdout purity — without a C++ host, a port, or timing. The
// mock returns known values so those checks can be exact; the e2e then proves
// the same server against the real debugger. Between them, a failure points at
// the layer that broke.
//
// The mock is deliberately dumb: two examples, a handful of trace events, an
// `input_set` that acks at epoch 9 and emits a tempo event of 246 (2 * 123, the
// same assertion the real e2e makes). It implements `DebugClientLike` — the
// same structural seam the real client satisfies.

import { toValue } from "../../../debug-client/src/index.ts";

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

import type { DebugClientLike } from "../../src/debug-client.ts";

/** The epoch the mock applies an input_set at. */
export const MOCK_APPLIED_EPOCH = 9n;

/** The value the mock emits on the clock after a bpm injection: 2 * 123. */
export const MOCK_TEMPO = 246;

function node(id: bigint, kind: string, clock: string, label: string, port: string, fanIn: bigint[] = []) {
  return { id, kind, clock, label, port, fanIn };
}

/** The `demo`-shaped example: two boundary inputs, two boundary outputs. */
export const MOCK_DEMO_GRAPH: GraphDescription = {
  name: "demo",
  nodes: [
    node(1n, "input", "ui", "input", "bpm"),
    node(2n, "internal", "audio", "lift", ""),
    node(3n, "output", "audio", "output", "tempo"),
    node(4n, "input", "ui", "input", "gain"),
    node(5n, "output", "ui", "output", "gain_out"),
  ],
  crossings: [{ from: 1n, to: 2n, policy: "latest" }],
  inputs: [
    { nodeId: 1n, name: "bpm" },
    { nodeId: 4n, name: "gain" },
  ],
  outputs: [
    { nodeId: 3n, name: "tempo" },
    { nodeId: 5n, name: "gain_out" },
  ],
  types: [],
};

/** The `daw`-shaped example, used to pin example_select + the moving resource. */
export const MOCK_DAW_GRAPH: GraphDescription = {
  name: "daw",
  nodes: [
    node(11n, "input", "ui", "input", "playing"),
    node(12n, "internal", "ui", "hold", ""),
    node(13n, "output", "audio", "output", "playhead"),
  ],
  crossings: [{ from: 12n, to: 13n, policy: "latest" }],
  inputs: [{ nodeId: 11n, name: "playing" }],
  outputs: [{ nodeId: 13n, name: "playhead" }],
  types: [],
};

function event(
  sequence: bigint,
  clock: string,
  epoch: bigint,
  kind: string,
  nodeId: bigint,
  value: Value | null,
): TraceEvent {
  return {
    sequence,
    stampNs: sequence * 1_000_000n,
    kind: kind as TraceEvent["kind"],
    nodeId,
    clock,
    epoch,
    value,
    threw: false,
    durationNs: 5_000n,
  };
}

/** The trace the mock "recorded" before the client attached. */
const MOCK_HISTORY: EventBatchView[] = [
  {
    clock: "audio",
    dropped: 0n,
    replay: true,
    events: [
      event(1n, "audio", 1n, "region_step_begin", 0n, null),
      event(2n, "audio", 1n, "node_tick_end", 2n, { kind: "double", value: 120 }),
      event(3n, "audio", 1n, "node_tick_end", 3n, { kind: "double", value: 120 }),
    ],
  },
  {
    clock: "ui",
    dropped: 0n,
    replay: true,
    events: [
      event(1n, "ui", 1n, "node_tick_end", 1n, { kind: "double", value: 60 }),
      event(2n, "ui", 1n, "node_tick_end", 4n, { kind: "double", value: 0.5 }),
    ],
  },
];

/**
 * A large, verbose trace for the output-cap tests.
 *
 * Set `MOCK_LARGE=1` in the MCP server's environment to select it: 500 audio +
 * 300 ui events (~600+ bytes each once serialised), diverse node ids and kinds,
 * periodic throws. The default history stays tiny so the shape tests remain
 * exact.
 */
function largeHistory(): EventBatchView[] {
  const value = "x".repeat(400);

  const audio: TraceEvent[] = [];
  const ui: TraceEvent[] = [];

  for (let sequence = 1; sequence <= 500; sequence += 1) {
    const kind = sequence % 50 === 0 ? "node_tick_throw" : "node_tick_end";

    audio.push(
      event(BigInt(sequence), "audio", BigInt(sequence), kind, BigInt(1 + (sequence % 3)), {
        kind: "string",
        value,
      }),
    );
  }

  for (let sequence = 1; sequence <= 300; sequence += 1) {
    const kind = sequence % 25 === 0 ? "node_tick_throw" : "node_tick_end";

    ui.push(
      event(BigInt(sequence), "ui", BigInt(sequence), kind, BigInt(4 + (sequence % 2)), {
        kind: "string",
        value,
      }),
    );
  }

  // Mark throws consistently with the kind.
  for (const entry of [...audio, ...ui]) entry.threw = entry.kind === "node_tick_throw";

  return [
    { clock: "audio", dropped: 0n, replay: true, events: audio },
    { clock: "ui", dropped: 0n, replay: true, events: ui },
  ];
}

const LARGE_HISTORY = process.env.MOCK_LARGE === "1" ? largeHistory() : null;

interface Subscriber {
  since: Map<string, bigint>;
  queue: EventBatchView[];
  waiters: Array<(batch: EventBatchView | null) => void>;
  ended: boolean;
}

/** The mock client class; exported for tests that want to drive it directly. */
export class MockClient implements DebugClientLike {
  #state: ConnectionState = "idle";
  #graph: GraphDescription | null = null;
  #examples: ExampleList = { names: ["demo", "daw"], active: "demo" };
  #history: EventBatchView[] = LARGE_HISTORY ?? [...MOCK_HISTORY];
  #cursors = new Map<string, bigint>();
  #dropped = new Map<string, bigint>([["ui", 2n]]);
  #subscribers = new Set<Subscriber>();
  #lastInput: { port: string; value: Value } | null = null;
  #pauseCalls = 0;
  #resumeCalls = 0;

  get state(): ConnectionState {
    return this.#state;
  }

  get graph(): GraphDescription | null {
    return this.#graph;
  }

  get currentExample(): string {
    return this.#graph?.name !== undefined && this.#graph.name !== "" ? this.#graph.name : this.#examples.active;
  }

  get cursors(): Record<string, bigint> {
    return Object.fromEntries(this.#cursors);
  }

  get dropped(): DropStats {
    return { events: Object.fromEntries(this.#dropped), commands: 0n };
  }

  get pauseCalls(): number {
    return this.#pauseCalls;
  }

  get resumeCalls(): number {
    return this.#resumeCalls;
  }

  async connect(): Promise<GraphDescription> {
    if (this.#graph === null) this.#graph = this.#examples.active === "daw" ? MOCK_DAW_GRAPH : MOCK_DEMO_GRAPH;
    this.#state = "open";
    return this.#graph;
  }

  close(): void {
    this.#state = "closed";

    for (const subscriber of this.#subscribers) {
      subscriber.ended = true;

      for (const waiter of subscriber.waiters.splice(0)) waiter(null);
    }

    this.#subscribers.clear();
  }

  async *events(options: EventsOptions = {}): AsyncGenerator<EventBatchView, void, void> {
    const live = options.live !== false;
    const since = new Map<string, bigint>();

    if (options.sinceSequence !== undefined) {
      for (const [clock, sequence] of Object.entries(options.sinceSequence)) since.set(clock, BigInt(sequence));
    } else {
      for (const [clock, sequence] of this.#cursors) since.set(clock, sequence);
    }

    const subscriber: Subscriber = { since, queue: [], waiters: [], ended: false };

    this.#subscribers.add(subscriber);

    try {
      for (const batch of this.#history) {
        const fresh = this.#fresh(subscriber, batch);

        if (fresh === null) continue;

        yield fresh;
      }

      if (!live) return;

      for (;;) {
        if (options.signal?.aborted) return;

        const batch = await this.#next(subscriber, options.signal);

        if (batch === null) return;

        const fresh = this.#fresh(subscriber, batch);

        if (fresh !== null) yield fresh;
      }
    } finally {
      subscriber.ended = true;
      this.#subscribers.delete(subscriber);

      for (const waiter of subscriber.waiters.splice(0)) waiter(null);
    }
  }

  async setInput(port: string, value: unknown, _options: RequestOptions = {}): Promise<SetInputResult> {
    // The server hands the client the caller's ValueInput; canonicalise it the
    // way the real client's encoder would, so a read-back has the wire shape.
    this.#lastInput = { port, value: toValue(value as ValueInput) };

    // Emit the effect shortly after the ack, the way a host applies a queued
    // command at its next tick boundary: the watch/tail tests then see it.
    setTimeout(() => {
      const sequence = (this.#cursors.get("audio") ?? 0n) + 1n;

      this.#cursors.set("audio", sequence);

      const batch: EventBatchView = {
        clock: "audio",
        dropped: 0n,
        replay: false,
        events: [
          event(sequence, "audio", MOCK_APPLIED_EPOCH, "node_tick_end", 3n, {
            kind: "double",
            value: MOCK_TEMPO,
          }),
        ],
      };

      this.#history.push(batch);
      this.#emit(batch);
    }, 5);

    return { requestId: 1n, appliedEpoch: MOCK_APPLIED_EPOCH };
  }

  async getInput(port: string, _options: RequestOptions = {}): Promise<PortValue> {
    const value =
      this.#lastInput !== null && this.#lastInput.port === port
        ? this.#lastInput.value
        : ({ kind: "double", value: 60 } as Value);

    return { requestId: 2n, port, value, error: "" };
  }

  async getOutput(port: string, _options: RequestOptions = {}): Promise<PortValue> {
    return { requestId: 3n, port, value: { kind: "double", value: MOCK_TEMPO }, error: "" };
  }

  async examples(): Promise<ExampleList> {
    return { names: [...this.#examples.names], active: this.currentExample };
  }

  async selectExample(name: string): Promise<void> {
    if (!this.#examples.names.includes(name)) {
      const error = new Error(`unknown example "${name}" (known: ${this.#examples.names.join(", ")})`);

      (error as Error & { code?: string }).code = "unknown_example";
      throw error;
    }

    this.#examples.active = name;
    this.#graph = name === "daw" ? MOCK_DAW_GRAPH : MOCK_DEMO_GRAPH;
  }

  async pause(): Promise<void> {
    this.#pauseCalls += 1;
  }

  async resume(): Promise<void> {
    this.#resumeCalls += 1;
  }

  // --- internals -----------------------------------------------------------

  #fresh(subscriber: Subscriber, batch: EventBatchView): EventBatchView | null {
    const cursor = subscriber.since.get(batch.clock);
    const events = cursor === undefined ? batch.events : batch.events.filter((entry) => entry.sequence > cursor);

    if (events.length === 0 && batch.dropped === 0n) return null;

    if (events.length > 0) {
      subscriber.since.set(batch.clock, events[events.length - 1]!.sequence);
      this.#cursors.set(batch.clock, events[events.length - 1]!.sequence);
    }

    return { clock: batch.clock, dropped: batch.dropped, events, replay: batch.replay };
  }

  #emit(batch: EventBatchView): void {
    for (const subscriber of [...this.#subscribers]) {
      if (subscriber.ended) continue;

      const waiter = subscriber.waiters.shift();

      if (waiter !== undefined) waiter(batch);
      else subscriber.queue.push(batch);
    }
  }

  #next(subscriber: Subscriber, signal?: AbortSignal): Promise<EventBatchView | null> {
    const queued = subscriber.queue.shift();

    if (queued !== undefined) return Promise.resolve(queued);
    if (subscriber.ended) return Promise.resolve(null);

    return new Promise((resolveNext) => {
      const waiter = (batch: EventBatchView | null): void => {
        signal?.removeEventListener("abort", onAbort);
        resolveNext(batch);
      };

      const onAbort = (): void => {
        const index = subscriber.waiters.indexOf(waiter);

        if (index >= 0) subscriber.waiters.splice(index, 1);

        resolveNext(null);
      };

      subscriber.waiters.push(waiter);
      signal?.addEventListener("abort", onAbort, { once: true });
    });
  }
}

/** `createClient` for main.ts's `--client-module` seam; the URL is unused. */
export function createClient(_url?: string): DebugClientLike {
  return new MockClient();
}
